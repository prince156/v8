// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_MARK_COMPACT_H_
#define V8_HEAP_MARK_COMPACT_H_

#include <deque>
#include <vector>

#include "src/base/bits.h"
#include "src/heap/marking.h"
#include "src/heap/sequential-marking-deque.h"
#include "src/heap/spaces.h"
#include "src/heap/worklist.h"

namespace v8 {
namespace internal {

// Forward declarations.
class EvacuationJobTraits;
class HeapObjectVisitor;
class ItemParallelJob;
class MigrationObserver;
class RecordMigratedSlotVisitor;
class YoungGenerationMarkingVisitor;
class Worklist;
class WorklistView;

class ObjectMarking : public AllStatic {
 public:
  V8_INLINE static MarkBit MarkBitFrom(HeapObject* obj,
                                       const MarkingState& state) {
    const Address address = obj->address();
    const MemoryChunk* p = MemoryChunk::FromAddress(address);
    return state.bitmap()->MarkBitFromIndex(p->AddressToMarkbitIndex(address));
  }

  static Marking::ObjectColor Color(HeapObject* obj,
                                    const MarkingState& state) {
    return Marking::Color(ObjectMarking::MarkBitFrom(obj, state));
  }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  V8_INLINE static bool IsImpossible(HeapObject* obj,
                                     const MarkingState& state) {
    return Marking::IsImpossible<access_mode>(MarkBitFrom(obj, state));
  }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  V8_INLINE static bool IsBlack(HeapObject* obj, const MarkingState& state) {
    return Marking::IsBlack<access_mode>(MarkBitFrom(obj, state));
  }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  V8_INLINE static bool IsWhite(HeapObject* obj, const MarkingState& state) {
    return Marking::IsWhite<access_mode>(MarkBitFrom(obj, state));
  }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  V8_INLINE static bool IsGrey(HeapObject* obj, const MarkingState& state) {
    return Marking::IsGrey<access_mode>(MarkBitFrom(obj, state));
  }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  V8_INLINE static bool IsBlackOrGrey(HeapObject* obj,
                                      const MarkingState& state) {
    return Marking::IsBlackOrGrey<access_mode>(MarkBitFrom(obj, state));
  }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  V8_INLINE static bool BlackToGrey(HeapObject* obj,
                                    const MarkingState& state) {
    MarkBit markbit = MarkBitFrom(obj, state);
    if (!Marking::BlackToGrey<access_mode>(markbit)) return false;
    state.IncrementLiveBytes<access_mode>(-obj->Size());
    return true;
  }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  V8_INLINE static bool WhiteToGrey(HeapObject* obj,
                                    const MarkingState& state) {
    return Marking::WhiteToGrey<access_mode>(MarkBitFrom(obj, state));
  }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  V8_INLINE static bool WhiteToBlack(HeapObject* obj,
                                     const MarkingState& state) {
    return ObjectMarking::WhiteToGrey<access_mode>(obj, state) &&
           ObjectMarking::GreyToBlack<access_mode>(obj, state);
  }

  template <AccessMode access_mode = AccessMode::NON_ATOMIC>
  V8_INLINE static bool GreyToBlack(HeapObject* obj,
                                    const MarkingState& state) {
    MarkBit markbit = MarkBitFrom(obj, state);
    if (!Marking::GreyToBlack<access_mode>(markbit)) return false;
    state.IncrementLiveBytes<access_mode>(obj->Size());
    return true;
  }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(ObjectMarking);
};

class MarkBitCellIterator {
 public:
  MarkBitCellIterator(MemoryChunk* chunk, MarkingState state) : chunk_(chunk) {
    DCHECK(Bitmap::IsCellAligned(
        chunk_->AddressToMarkbitIndex(chunk_->area_start())));
    DCHECK(Bitmap::IsCellAligned(
        chunk_->AddressToMarkbitIndex(chunk_->area_end())));
    last_cell_index_ =
        Bitmap::IndexToCell(chunk_->AddressToMarkbitIndex(chunk_->area_end()));
    cell_base_ = chunk_->area_start();
    cell_index_ =
        Bitmap::IndexToCell(chunk_->AddressToMarkbitIndex(cell_base_));
    cells_ = state.bitmap()->cells();
  }

  inline bool Done() { return cell_index_ >= last_cell_index_; }

  inline bool HasNext() { return cell_index_ < last_cell_index_ - 1; }

  inline MarkBit::CellType* CurrentCell() {
    DCHECK_EQ(cell_index_, Bitmap::IndexToCell(Bitmap::CellAlignIndex(
                               chunk_->AddressToMarkbitIndex(cell_base_))));
    return &cells_[cell_index_];
  }

  inline Address CurrentCellBase() {
    DCHECK_EQ(cell_index_, Bitmap::IndexToCell(Bitmap::CellAlignIndex(
                               chunk_->AddressToMarkbitIndex(cell_base_))));
    return cell_base_;
  }

  MUST_USE_RESULT inline bool Advance() {
    cell_base_ += Bitmap::kBitsPerCell * kPointerSize;
    return ++cell_index_ != last_cell_index_;
  }

  inline bool Advance(unsigned int new_cell_index) {
    if (new_cell_index != cell_index_) {
      DCHECK_GT(new_cell_index, cell_index_);
      DCHECK_LE(new_cell_index, last_cell_index_);
      unsigned int diff = new_cell_index - cell_index_;
      cell_index_ = new_cell_index;
      cell_base_ += diff * (Bitmap::kBitsPerCell * kPointerSize);
      return true;
    }
    return false;
  }

  // Return the next mark bit cell. If there is no next it returns 0;
  inline MarkBit::CellType PeekNext() {
    if (HasNext()) {
      return cells_[cell_index_ + 1];
    }
    return 0;
  }

 private:
  MemoryChunk* chunk_;
  MarkBit::CellType* cells_;
  unsigned int last_cell_index_;
  unsigned int cell_index_;
  Address cell_base_;
};

enum LiveObjectIterationMode {
  kBlackObjects,
  kGreyObjects,
  kAllLiveObjects
};

template <LiveObjectIterationMode mode>
class LiveObjectRange {
 public:
  class iterator {
   public:
    using value_type = std::pair<HeapObject*, int /* size */>;
    using pointer = const value_type*;
    using reference = const value_type&;
    using iterator_category = std::forward_iterator_tag;

    inline iterator(MemoryChunk* chunk, MarkingState state, Address start);

    inline iterator& operator++();
    inline iterator operator++(int);

    bool operator==(iterator other) const {
      return current_object_ == other.current_object_;
    }

    bool operator!=(iterator other) const { return !(*this == other); }

    value_type operator*() {
      return std::make_pair(current_object_, current_size_);
    }

   private:
    inline void AdvanceToNextValidObject();

    MemoryChunk* const chunk_;
    Map* const one_word_filler_map_;
    Map* const two_word_filler_map_;
    Map* const free_space_map_;
    MarkBitCellIterator it_;
    Address cell_base_;
    MarkBit::CellType current_cell_;
    HeapObject* current_object_;
    int current_size_;
  };

  LiveObjectRange(MemoryChunk* chunk, MarkingState state)
      : chunk_(chunk),
        state_(state),
        start_(chunk_->area_start()),
        end_(chunk->area_end()) {}

  inline iterator begin();
  inline iterator end();

 private:
  MemoryChunk* const chunk_;
  MarkingState state_;
  Address start_;
  Address end_;
};

class LiveObjectVisitor : AllStatic {
 public:
  enum IterationMode {
    kKeepMarking,
    kClearMarkbits,
  };

  // Visits black objects on a MemoryChunk until the Visitor returns |false| for
  // an object. If IterationMode::kClearMarkbits is passed the markbits and
  // slots for visited objects are cleared for each successfully visited object.
  template <class Visitor>
  static bool VisitBlackObjects(MemoryChunk* chunk, const MarkingState& state,
                                Visitor* visitor, IterationMode iteration_mode,
                                HeapObject** failed_object);

  // Visits black objects on a MemoryChunk. The visitor is not allowed to fail
  // visitation for an object.
  template <class Visitor>
  static void VisitBlackObjectsNoFail(MemoryChunk* chunk,
                                      const MarkingState& state,
                                      Visitor* visitor,
                                      IterationMode iteration_mode);

  // Visits black objects on a MemoryChunk. The visitor is not allowed to fail
  // visitation for an object.
  template <class Visitor>
  static void VisitGreyObjectsNoFail(MemoryChunk* chunk,
                                     const MarkingState& state,
                                     Visitor* visitor,
                                     IterationMode iteration_mode);

  static void RecomputeLiveBytes(MemoryChunk* chunk, const MarkingState& state);
};

enum PageEvacuationMode { NEW_TO_NEW, NEW_TO_OLD };
enum FreeSpaceTreatmentMode { IGNORE_FREE_SPACE, ZAP_FREE_SPACE };
enum MarkingTreatmentMode { KEEP, CLEAR };
enum class RememberedSetUpdatingMode { ALL, OLD_TO_NEW_ONLY };

// Base class for minor and full MC collectors.
class MarkCompactCollectorBase {
 public:
  virtual ~MarkCompactCollectorBase() {}

  // Note: Make sure to refer to the instances by their concrete collector
  // type to avoid vtable lookups marking state methods when used in hot paths.
  virtual MarkingState marking_state(HeapObject* object) const = 0;
  virtual MarkingState marking_state(MemoryChunk* chunk) const = 0;

  virtual void SetUp() = 0;
  virtual void TearDown() = 0;
  virtual void CollectGarbage() = 0;

  inline Heap* heap() const { return heap_; }
  inline Isolate* isolate() { return heap()->isolate(); }

 protected:
  explicit MarkCompactCollectorBase(Heap* heap)
      : heap_(heap), old_to_new_slots_(0) {}

  // Marking operations for objects reachable from roots.
  virtual void MarkLiveObjects() = 0;
  // Mark objects reachable (transitively) from objects in the marking
  // stack.
  virtual void EmptyMarkingWorklist() = 0;
  virtual void ProcessMarkingWorklist() = 0;
  // Clear non-live references held in side data structures.
  virtual void ClearNonLiveReferences() = 0;
  virtual void EvacuatePrologue() = 0;
  virtual void EvacuateEpilogue() = 0;
  virtual void Evacuate() = 0;
  virtual void EvacuatePagesInParallel() = 0;
  virtual void UpdatePointersAfterEvacuation() = 0;

  template <class Evacuator, class Collector>
  void CreateAndExecuteEvacuationTasks(
      Collector* collector, ItemParallelJob* job,
      RecordMigratedSlotVisitor* record_visitor,
      MigrationObserver* migration_observer, const intptr_t live_bytes);

  // Returns whether this page should be moved according to heuristics.
  bool ShouldMovePage(Page* p, intptr_t live_bytes);

  int CollectToSpaceUpdatingItems(ItemParallelJob* job);
  int CollectRememberedSetUpdatingItems(ItemParallelJob* job,
                                        RememberedSetUpdatingMode mode);

  int NumberOfParallelCompactionTasks(int pages);
  int NumberOfParallelPointerUpdateTasks(int pages, int slots);
  int NumberOfParallelToSpacePointerUpdateTasks(int pages);

  Heap* heap_;
  // Number of old to new slots. Should be computed during MarkLiveObjects.
  // -1 indicates that the value couldn't be computed.
  int old_to_new_slots_;
};

// Collector for young-generation only.
class MinorMarkCompactCollector final : public MarkCompactCollectorBase {
 public:
  explicit MinorMarkCompactCollector(Heap* heap);
  ~MinorMarkCompactCollector();

  MarkingState marking_state(HeapObject* object) const override {
    return MarkingState::External(object);
  }

  MarkingState marking_state(MemoryChunk* chunk) const override {
    return MarkingState::External(chunk);
  }

  void SetUp() override;
  void TearDown() override;
  void CollectGarbage() override;

  void MakeIterable(Page* page, MarkingTreatmentMode marking_mode,
                    FreeSpaceTreatmentMode free_space_mode);
  void CleanupSweepToIteratePages();

 private:
  using MarkingWorklist = WorklistView;
  class RootMarkingVisitorSeedOnly;
  class RootMarkingVisitor;

  static const int kNumMarkers = 8;
  static const int kMainMarker = 0;

  inline Worklist* worklist() { return worklist_; }

  inline YoungGenerationMarkingVisitor* main_marking_visitor() {
    return main_marking_visitor_;
  }

  void MarkLiveObjects() override;
  void MarkRootSetInParallel();
  void ProcessMarkingWorklist() override;
  void EmptyMarkingWorklist() override;
  void ClearNonLiveReferences() override;

  void EvacuatePrologue() override;
  void EvacuateEpilogue() override;
  void Evacuate() override;
  void EvacuatePagesInParallel() override;
  void UpdatePointersAfterEvacuation() override;

  void CollectNewSpaceArrayBufferTrackerItems(ItemParallelJob* job);

  int NumberOfParallelMarkingTasks(int pages);

  Worklist* worklist_;
  YoungGenerationMarkingVisitor* main_marking_visitor_;
  base::Semaphore page_parallel_job_semaphore_;
  std::vector<Page*> new_space_evacuation_pages_;
  std::vector<Page*> sweep_to_iterate_pages_;

  friend class YoungGenerationMarkingTask;
  friend class YoungGenerationMarkingVisitor;
};

// Collector for young and old generation.
class MarkCompactCollector final : public MarkCompactCollectorBase {
 public:
#ifdef V8_CONCURRENT_MARKING
  // Wrapper for the shared and bailout worklists.
  class MarkingWorklist {
   public:
    static const int kMainThread = 0;
    // The heap parameter is not used but needed to match the sequential case.
    explicit MarkingWorklist(Heap* heap) {}

    bool Push(HeapObject* object) { return shared_.Push(kMainThread, object); }

    bool PushBailout(HeapObject* object) {
      return bailout_.Push(kMainThread, object);
    }

    HeapObject* Pop() {
      HeapObject* result;
      if (bailout_.Pop(kMainThread, &result)) return result;
      if (shared_.Pop(kMainThread, &result)) return result;
      return nullptr;
    }

    void Clear() {
      bailout_.Clear();
      shared_.Clear();
    }

    bool IsFull() { return false; }

    bool IsEmpty() {
      return bailout_.IsLocalEmpty(kMainThread) &&
             shared_.IsLocalEmpty(kMainThread) &&
             bailout_.IsGlobalPoolEmpty() && shared_.IsGlobalPoolEmpty();
    }

    int Size() {
      return static_cast<int>(bailout_.LocalSize(kMainThread) +
                              shared_.LocalSize(kMainThread));
    }

    // Calls the specified callback on each element of the deques and replaces
    // the element with the result of the callback. If the callback returns
    // nullptr then the element is removed from the deque.
    // The callback must accept HeapObject* and return HeapObject*.
    template <typename Callback>
    void Update(Callback callback) {
      bailout_.Update(callback);
      shared_.Update(callback);
    }

    Worklist* shared() { return &shared_; }
    Worklist* bailout() { return &bailout_; }

    // These empty functions are needed to match the interface
    // of the sequential marking deque.
    void SetUp() {}
    void TearDown() { Clear(); }
    void StartUsing() {}
    void StopUsing() {}
    void ClearOverflowed() {}
    void SetOverflowed() {}
    bool overflowed() const { return false; }

   private:
    Worklist shared_;
    Worklist bailout_;
  };
#else
  using MarkingWorklist = SequentialMarkingDeque;
#endif

  class RootMarkingVisitor;

  class Sweeper {
   public:
    class SweeperTask;

    enum FreeListRebuildingMode { REBUILD_FREE_LIST, IGNORE_FREE_LIST };
    enum ClearOldToNewSlotsMode {
      DO_NOT_CLEAR,
      CLEAR_REGULAR_SLOTS,
      CLEAR_TYPED_SLOTS
    };

    typedef std::deque<Page*> SweepingList;
    typedef std::vector<Page*> SweptList;

    static int RawSweep(Page* p, FreeListRebuildingMode free_list_mode,
                        FreeSpaceTreatmentMode free_space_mode);

    explicit Sweeper(Heap* heap)
        : heap_(heap),
          pending_sweeper_tasks_semaphore_(0),
          semaphore_counter_(0),
          sweeping_in_progress_(false),
          num_sweeping_tasks_(0) {}

    bool sweeping_in_progress() { return sweeping_in_progress_; }

    void AddPage(AllocationSpace space, Page* page);

    int ParallelSweepSpace(AllocationSpace identity, int required_freed_bytes,
                           int max_pages = 0);
    int ParallelSweepPage(Page* page, AllocationSpace identity);

    // After calling this function sweeping is considered to be in progress
    // and the main thread can sweep lazily, but the background sweeper tasks
    // are not running yet.
    void StartSweeping();
    void StartSweeperTasks();
    void EnsureCompleted();
    void EnsureNewSpaceCompleted();
    bool AreSweeperTasksRunning();
    void SweepOrWaitUntilSweepingCompleted(Page* page);

    void AddSweptPageSafe(PagedSpace* space, Page* page);
    Page* GetSweptPageSafe(PagedSpace* space);

   private:
    static const int kAllocationSpaces = LAST_PAGED_SPACE + 1;

    static ClearOldToNewSlotsMode GetClearOldToNewSlotsMode(Page* p);

    template <typename Callback>
    void ForAllSweepingSpaces(Callback callback) {
      for (int i = 0; i < kAllocationSpaces; i++) {
        callback(static_cast<AllocationSpace>(i));
      }
    }

    Page* GetSweepingPageSafe(AllocationSpace space);

    void PrepareToBeSweptPage(AllocationSpace space, Page* page);

    Heap* heap_;
    base::Semaphore pending_sweeper_tasks_semaphore_;
    // Counter is only used for waiting on the semaphore.
    intptr_t semaphore_counter_;
    base::Mutex mutex_;
    SweptList swept_list_[kAllocationSpaces];
    SweepingList sweeping_list_[kAllocationSpaces];
    bool sweeping_in_progress_;
    // Counter is actively maintained by the concurrent tasks to avoid querying
    // the semaphore for maintaining a task counter on the main thread.
    base::AtomicNumber<intptr_t> num_sweeping_tasks_;
  };

  enum IterationMode {
    kKeepMarking,
    kClearMarkbits,
  };

  static void Initialize();

  MarkingState marking_state(HeapObject* object) const override {
    return MarkingState::Internal(object);
  }

  MarkingState marking_state(MemoryChunk* chunk) const override {
    return MarkingState::Internal(chunk);
  }

  void SetUp() override;
  void TearDown() override;
  // Performs a global garbage collection.
  void CollectGarbage() override;

  void CollectEvacuationCandidates(PagedSpace* space);

  void AddEvacuationCandidate(Page* p);

  // Prepares for GC by resetting relocation info in old and map spaces and
  // choosing spaces to compact.
  void Prepare();

  bool StartCompaction();

  void AbortCompaction();

  INLINE(static bool ShouldSkipEvacuationSlotRecording(Object* host)) {
    return Page::FromAddress(reinterpret_cast<Address>(host))
        ->ShouldSkipEvacuationSlotRecording();
  }

  static inline bool IsOnEvacuationCandidate(HeapObject* obj) {
    return Page::FromAddress(reinterpret_cast<Address>(obj))
        ->IsEvacuationCandidate();
  }

  void RecordRelocSlot(Code* host, RelocInfo* rinfo, Object* target);
  void RecordCodeEntrySlot(HeapObject* host, Address slot, Code* target);
  void RecordCodeTargetPatch(Address pc, Code* target);
  INLINE(void RecordSlot(HeapObject* object, Object** slot, Object* target));
  INLINE(void ForceRecordSlot(HeapObject* object, Object** slot,
                              Object* target));
  void RecordLiveSlotsOnPage(Page* page);

  void UpdateSlots(SlotsBuffer* buffer);
  void UpdateSlotsRecordedIn(SlotsBuffer* buffer);

  void InvalidateCode(Code* code);

  void ClearMarkbits();

  bool is_compacting() const { return compacting_; }

  // Ensures that sweeping is finished.
  //
  // Note: Can only be called safely from main thread.
  void EnsureSweepingCompleted();

  // Help out in sweeping the corresponding space and refill memory that has
  // been regained.
  //
  // Note: Thread-safe.
  void SweepAndRefill(CompactionSpace* space);

  // Checks if sweeping is in progress right now on any space.
  bool sweeping_in_progress() { return sweeper().sweeping_in_progress(); }

  void set_evacuation(bool evacuation) { evacuation_ = evacuation; }

  bool evacuation() const { return evacuation_; }

  MarkingWorklist* marking_worklist() { return &marking_worklist_; }

  Sweeper& sweeper() { return sweeper_; }

#ifdef DEBUG
  // Checks whether performing mark-compact collection.
  bool in_use() { return state_ > PREPARE_GC; }
  bool are_map_pointers_encoded() { return state_ == UPDATE_POINTERS; }
#endif

#ifdef VERIFY_HEAP
  void VerifyValidStoreAndSlotsBufferEntries();
  void VerifyMarkbitsAreClean();
  static void VerifyMarkbitsAreClean(PagedSpace* space);
  static void VerifyMarkbitsAreClean(NewSpace* space);
  void VerifyWeakEmbeddedObjectsInCode();
  void VerifyOmittedMapChecks();
#endif

 private:
  explicit MarkCompactCollector(Heap* heap);

  bool WillBeDeoptimized(Code* code);

  void ComputeEvacuationHeuristics(size_t area_size,
                                   int* target_fragmentation_percent,
                                   size_t* max_evacuated_bytes);

  void VisitAllObjects(HeapObjectVisitor* visitor);

  void RecordObjectStats();

  // Finishes GC, performs heap verification if enabled.
  void Finish();

  void MarkLiveObjects() override;

  // Pushes a black object onto the marking stack and accounts for live bytes.
  // Note that this assumes live bytes have not yet been counted.
  V8_INLINE void PushBlack(HeapObject* obj);

  // Marks the object black and pushes it on the marking stack.
  // This is for non-incremental marking only.
  V8_INLINE void MarkObject(HeapObject* obj);

  // Mark the heap roots and all objects reachable from them.
  void MarkRoots(RootMarkingVisitor* visitor);

  // Mark the string table specially.  References to internalized strings from
  // the string table are weak.
  void MarkStringTable(RootMarkingVisitor* visitor);

  void ProcessMarkingWorklist() override;

  // Mark objects reachable (transitively) from objects in the marking stack
  // or overflowed in the heap.  This respects references only considered in
  // the final atomic marking pause including the following:
  //    - Processing of objects reachable through Harmony WeakMaps.
  //    - Objects reachable due to host application logic like object groups,
  //      implicit references' groups, or embedder heap tracing.
  void ProcessEphemeralMarking(bool only_process_harmony_weak_collections);

  // If the call-site of the top optimized code was not prepared for
  // deoptimization, then treat the maps in the code as strong pointers,
  // otherwise a map can die and deoptimize the code.
  void ProcessTopOptimizedFrame(RootMarkingVisitor* visitor);

  // Collects a list of dependent code from maps embedded in optimize code.
  DependentCode* DependentCodeListFromNonLiveMaps();

  // This function empties the marking stack, but may leave overflowed objects
  // in the heap, in which case the marking stack's overflow flag will be set.
  void EmptyMarkingWorklist() override;

  // Refill the marking stack with overflowed objects from the heap.  This
  // function either leaves the marking stack full or clears the overflow
  // flag on the marking stack.
  void RefillMarkingWorklist();

  // Helper methods for refilling the marking stack by discovering grey objects
  // on various pages of the heap. Used by {RefillMarkingWorklist} only.
  template <class T>
  void DiscoverGreyObjectsWithIterator(T* it);
  void DiscoverGreyObjectsOnPage(MemoryChunk* p);
  void DiscoverGreyObjectsInSpace(PagedSpace* space);
  void DiscoverGreyObjectsInNewSpace();

  // Callback function for telling whether the object *p is an unmarked
  // heap object.
  static bool IsUnmarkedHeapObject(Object** p);

  // Clear non-live references in weak cells, transition and descriptor arrays,
  // and deoptimize dependent code of non-live maps.
  void ClearNonLiveReferences() override;
  void MarkDependentCodeForDeoptimization(DependentCode* list);
  // Find non-live targets of simple transitions in the given list. Clear
  // transitions to non-live targets and if needed trim descriptors arrays.
  void ClearSimpleMapTransitions(Object* non_live_map_list);
  void ClearSimpleMapTransition(Map* map, Map* dead_transition);
  // Compact every array in the global list of transition arrays and
  // trim the corresponding descriptor array if a transition target is non-live.
  void ClearFullMapTransitions();
  bool CompactTransitionArray(Map* map, TransitionArray* transitions,
                              DescriptorArray* descriptors);
  void TrimDescriptorArray(Map* map, DescriptorArray* descriptors);
  void TrimEnumCache(Map* map, DescriptorArray* descriptors);

  // Mark all values associated with reachable keys in weak collections
  // encountered so far.  This might push new object or even new weak maps onto
  // the marking stack.
  void ProcessWeakCollections();

  // After all reachable objects have been marked those weak map entries
  // with an unreachable key are removed from all encountered weak maps.
  // The linked list of all encountered weak maps is destroyed.
  void ClearWeakCollections();

  // We have to remove all encountered weak maps from the list of weak
  // collections when incremental marking is aborted.
  void AbortWeakCollections();

  void ClearWeakCells(Object** non_live_map_list,
                      DependentCode** dependent_code_list);
  void AbortWeakCells();

  void AbortTransitionArrays();

  // Starts sweeping of spaces by contributing on the main thread and setting
  // up other pages for sweeping. Does not start sweeper tasks.
  void StartSweepSpaces();
  void StartSweepSpace(PagedSpace* space);

  void EvacuatePrologue() override;
  void EvacuateEpilogue() override;
  void Evacuate() override;
  void EvacuatePagesInParallel() override;
  void UpdatePointersAfterEvacuation() override;

  void CollectNewSpaceArrayBufferTrackerItems(ItemParallelJob* job);
  void CollectOldSpaceArrayBufferTrackerItems(ItemParallelJob* job);

  void ReleaseEvacuationCandidates();
  void PostProcessEvacuationCandidates();
  void ReportAbortedEvacuationCandidate(HeapObject* failed_object, Page* page);

  base::Mutex mutex_;
  base::Semaphore page_parallel_job_semaphore_;

#ifdef DEBUG
  enum CollectorState {
    IDLE,
    PREPARE_GC,
    MARK_LIVE_OBJECTS,
    SWEEP_SPACES,
    ENCODE_FORWARDING_ADDRESSES,
    UPDATE_POINTERS,
    RELOCATE_OBJECTS
  };

  // The current stage of the collector.
  CollectorState state_;
#endif

  bool was_marked_incrementally_;

  bool evacuation_;

  // True if we are collecting slots to perform evacuation from evacuation
  // candidates.
  bool compacting_;

  bool black_allocation_;

  bool have_code_to_deoptimize_;

  MarkingWorklist marking_worklist_;

  // Candidates for pages that should be evacuated.
  std::vector<Page*> evacuation_candidates_;
  // Pages that are actually processed during evacuation.
  std::vector<Page*> old_space_evacuation_pages_;
  std::vector<Page*> new_space_evacuation_pages_;
  std::vector<std::pair<HeapObject*, Page*>> aborted_evacuation_candidates_;

  Sweeper sweeper_;

  friend class FullEvacuator;
  friend class Heap;
  friend class IncrementalMarkingMarkingVisitor;
  friend class MarkCompactMarkingVisitor;
  friend class RecordMigratedSlotVisitor;
};

class EvacuationScope {
 public:
  explicit EvacuationScope(MarkCompactCollector* collector)
      : collector_(collector) {
    collector_->set_evacuation(true);
  }

  ~EvacuationScope() { collector_->set_evacuation(false); }

 private:
  MarkCompactCollector* collector_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_MARK_COMPACT_H_
