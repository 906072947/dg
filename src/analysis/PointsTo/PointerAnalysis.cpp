#include "Pointer.h"
#include "PointerSubgraph.h"
#include "PointerAnalysis.h"

namespace dg {
namespace analysis {
namespace pta {

// nodes representing NULL, unknown memory
// and invalidated memory
PSNode NULLPTR_LOC(PSNodeType::NULL_ADDR);
PSNode *NULLPTR = &NULLPTR_LOC;
PSNode UNKNOWN_MEMLOC(PSNodeType::UNKNOWN_MEM);
PSNode *UNKNOWN_MEMORY = &UNKNOWN_MEMLOC;
PSNode INVALIDATED_LOC(PSNodeType::INVALIDATED);
PSNode *INVALIDATED = &INVALIDATED_LOC;

// pointers to those memory
const Pointer PointerUnknown(UNKNOWN_MEMORY, Offset::UNKNOWN);
const Pointer PointerNull(NULLPTR, 0);

// replace all pointers to given target with one
// to that target, but Offset::UNKNOWN
bool PSNode::addPointsToUnknownOffset(PSNode *target)
{
    bool changed = false;
    for (auto I = pointsTo.begin(), E = pointsTo.end(); I != E;) {
        auto cur = I++;

        // erase pointers to the same memory but with concrete offset
        if (cur->target == target && !cur->offset.isUnknown()) {
            pointsTo.erase(cur);
            changed = true;
        }
    }

    // DONT use addPointsTo() method, it would recursively call
    // this method again, until stack overflow
    changed |= pointsTo.insert(Pointer(target, Offset::UNKNOWN)).second;

    return changed;
}

bool PointerAnalysis::processLoad(PSNode *node)
{
    bool changed = false;
    PSNode *operand = node->getOperand(0);

    if (operand->pointsTo.empty())
        return error(operand, "Load's operand has no points-to set");

    for (const Pointer& ptr : operand->pointsTo) {
        // XXX: should this yield also UNKNOWN pointer
        if (!ptr.isValid() || ptr.isInvalidated())
            continue;

        if (ptr.isUnknown()) {
            // load from unknown pointer yields unknown pointer
            changed |= node->addPointsTo(UNKNOWN_MEMORY);
            continue;
        }

        // find memory objects holding relevant points-to
        // information
        std::vector<MemoryObject *> objects;
        getMemoryObjects(node, ptr, objects);

        PSNodeAlloc *target = PSNodeAlloc::get(ptr.target);
        assert(target && "Target is not memory allocation");

        // no objects found for this target? That is
        // load from unknown memory
        if (objects.empty()) {
            if (target->isZeroInitialized())
                // if the memory is zero initialized, then everything
                // is fine, we add nullptr
                changed |= node->addPointsTo(NULLPTR);
            else
                changed |= errorEmptyPointsTo(node, target);

            continue;
        }

        for (MemoryObject *o : objects) {
            // is the offset to the memory unknown?
            // In that case everything can be referenced,
            // so we need to copy the whole points-to
            if (ptr.offset.isUnknown()) {
                // we should load from memory that has
                // no pointers in it - it may be an error
                // FIXME: don't duplicate the code
                if (o->pointsTo.empty()) {
                    if (target->isZeroInitialized())
                        changed |= node->addPointsTo(NULLPTR);
                    else if (objects.size() == 1)
                        changed |= errorEmptyPointsTo(node, target);
                }

                // we have some pointers - copy them all,
                // since the offset is unknown
                for (auto& it : o->pointsTo) {
                    for (const Pointer &p : it.second) {
                        changed |= node->addPointsTo(p);
                    }
                }

                // this is all that we can do here...
                continue;
            }

            // load from empty points-to set
            // - that is load from unknown memory
            if (!o->pointsTo.count(ptr.offset)) {
                // if the memory is zero initialized, then everything
                // is fine, we add nullptr
                if (target->isZeroInitialized())
                    changed |= node->addPointsTo(NULLPTR);
                // if we don't have a definition even with unknown offset
                // it is an error
                // FIXME: don't triplicate the code!
                else if (!o->pointsTo.count(Offset::UNKNOWN))
                    changed |= errorEmptyPointsTo(node, target);
            } else {
                // we have pointers on that memory, so we can
                // do the work
                for (const Pointer& memptr : o->pointsTo[ptr.offset])
                    changed |= node->addPointsTo(memptr);
            }

            // plus always add the pointers at unknown offset,
            // since these can be what we need too
            if (o->pointsTo.count(Offset::UNKNOWN)) {
                for (const Pointer& memptr : o->pointsTo[Offset::UNKNOWN]) {
                    changed |= node->addPointsTo(memptr);
                }
            }
        }
    }

    return changed;
}

bool PointerAnalysis::processMemcpy(PSNode *node)
{
    bool changed = false;
    PSNodeMemcpy *memcpy = PSNodeMemcpy::get(node);
    PSNode *srcNode = memcpy->getSource();
    PSNode *destNode = memcpy->getDestination();

    std::vector<MemoryObject *> srcObjects;
    std::vector<MemoryObject *> destObjects;

    // gather srcNode pointer objects
    for (const Pointer& ptr : srcNode->pointsTo) {
        assert(ptr.target && "Got nullptr as target");

        if (!ptr.isValid() || ptr.isInvalidated())
            continue;

        srcObjects.clear();
        getMemoryObjects(node, ptr, srcObjects);

        if (srcObjects.empty()){
            abort();
            return changed;
        }

        // gather destNode objects
        for (const Pointer& dptr : destNode->pointsTo) {
            assert(dptr.target && "Got nullptr as target");

            if (!dptr.isValid() || dptr.isInvalidated())
                continue;

            destObjects.clear();
            getMemoryObjects(node, dptr, destObjects);

            if (destObjects.empty()) {
                abort();
                return changed;
            }

            changed |= processMemcpy(srcObjects, destObjects,
                                     ptr, dptr,
                                     memcpy->getLength());
        }
    }

    return changed;
}

bool PointerAnalysis::processMemcpy(std::vector<MemoryObject *>& srcObjects,
                                    std::vector<MemoryObject *>& destObjects,
                                    const Pointer& sptr, const Pointer& dptr,
                                    Offset len)
{
    bool changed = false;
    Offset srcOffset = sptr.offset;
    Offset destOffset = dptr.offset;

    assert(*len > 0 && "Memcpy of length 0");

    PSNodeAlloc *sourceAlloc = PSNodeAlloc::get(sptr.target);
    assert(sourceAlloc && "Pointer's target in memcpy is not an allocation");
    PSNodeAlloc *destAlloc = PSNodeAlloc::get(dptr.target);
    assert(destAlloc && "Pointer's target in memcpy is not an allocation");

    // set to true if the contents of destination memory
    // can contain null
    bool contains_null_somewhere = false;

    // if the source is zero initialized, we may copy null pointer
    if (sourceAlloc->isZeroInitialized()) {
        // if we really copy the whole object, just set it zero-initialized
        if ((sourceAlloc->getSize() != Offset::UNKNOWN) &&
            (sourceAlloc->getSize() == destAlloc->getSize()) &&
            len == sourceAlloc->getSize() && sptr.offset == 0) {
            destAlloc->setZeroInitialized();
        } else {
            // we could analyze in a lot of cases where
            // shoulde be stored the nullptr, but the question
            // is whether it is worth it... For now, just say
            // that somewhere may be null in the destination
            contains_null_somewhere = true;
        }
    }

    for (MemoryObject *destO : destObjects) {
        if (contains_null_somewhere)
            changed |= destO->addPointsTo(Offset::UNKNOWN, NULLPTR);

        // copy every pointer from srcObjects that is in
        // the range to destination's objects
        for (MemoryObject *so : srcObjects) {
            for (auto& src : so->pointsTo) { // src.first is offset,
                                             // src.second is a PointToSet

                // if the offset is inbound of the copied memory
                // or we copy from unknown offset, or this pointer
                // is on unknown offset, copy this pointer
                if (src.first.isUnknown() ||
                    srcOffset.isUnknown() ||
                    (srcOffset <= src.first &&
                     (len.isUnknown() ||
                      *src.first - *srcOffset < *len))) {

                    // copy the pointer, but shift it by the offsets
                    // we are working with
                    if (!src.first.isUnknown() && !srcOffset.isUnknown() &&
                        !destOffset.isUnknown()) {
                        // check that new offset does not overflow Offset::UNKNOWN
                        if (Offset::UNKNOWN - *destOffset <= *src.first - *srcOffset) {
                            changed |= destO->addPointsTo(Offset::UNKNOWN, src.second);
                            continue;
                        }

                        Offset newOff = *src.first - *srcOffset + *destOffset;
                        if (newOff >= destO->node->getSize() ||
                            newOff >= max_offset) {
                            changed |= destO->addPointsTo(Offset::UNKNOWN, src.second);
                        } else {
                            changed |= destO->addPointsTo(newOff, src.second);
                        }
                    } else {
                        changed |= destO->addPointsTo(Offset::UNKNOWN, src.second);
                    }
                }
            }
        }
    }

    return changed;
}

bool PointerAnalysis::processGep(PSNode *node) {
    bool changed = false;

    PSNodeGep *gep = PSNodeGep::get(node);
    assert(gep && "Non-GEP given");

    for (const Pointer& ptr : gep->getSource()->pointsTo) {
        uint64_t new_offset;
        if (ptr.offset.isUnknown() || gep->getOffset().isUnknown())
            // set it like this to avoid overflow when adding
            new_offset = Offset::UNKNOWN;
        else
            new_offset = *ptr.offset + *gep->getOffset();

        // in the case PSNodeType::the memory has size 0, then every pointer
        // will have unknown offset with the exception that it points
        // to the begining of the memory - therefore make 0 exception
        if ((new_offset == 0 || new_offset < ptr.target->getSize())
            && new_offset < max_offset)
            changed |= node->addPointsTo(ptr.target, new_offset);
        else
            changed |= node->addPointsToUnknownOffset(ptr.target);
    }

    return changed;
}

bool PointerAnalysis::processNode(PSNode *node)
{
    bool changed = false;
    std::vector<MemoryObject *> objects;

#ifdef DEBUG_ENABLED
    size_t prev_size = node->pointsTo.size();
#endif

    switch(node->type) {
        case PSNodeType::LOAD:
            changed |= processLoad(node);
            break;
        case PSNodeType::STORE:
            for (const Pointer& ptr : node->getOperand(1)->pointsTo) {
                assert(ptr.target && "Got nullptr as target");

                if (ptr.isNull())
                    continue;

                objects.clear();
                getMemoryObjects(node, ptr, objects);
                for (MemoryObject *o : objects) {
                    for (const Pointer& to : node->getOperand(0)->pointsTo) {
                        changed |= o->addPointsTo(ptr.offset, to);
                    }
                }
            }
            break;
        case PSNodeType::FREE:
            break;
        case PSNodeType::INVALIDATE_LOCALS:
            // FIXME: get rid of this type of node
            // (make the analysis extendable and move it there)
            node->setParent(node->getOperand(0)->getSingleSuccessor()->getParent());
            break;
        case PSNodeType::GEP:
            changed |= processGep(node);
            break;
        case PSNodeType::CAST:
            // cast only copies the pointers
            for (const Pointer& ptr : node->getOperand(0)->pointsTo)
                changed |= node->addPointsTo(ptr);
            break;
        case PSNodeType::CONSTANT:
            // maybe warn? It has no sense to insert the constants into the graph.
            // On the other hand it is harmless. We can at least check if it is
            // correctly initialized 8-)
            assert(node->pointsTo.size() == 1
                   && "Constant should have exactly one pointer");
            break;
        case PSNodeType::CALL_RETURN:
            if (invalidate_nodes) {
                for (PSNode *op : node->operands) {
                    for (const Pointer& ptr : op->pointsTo) {
                        PSNodeAlloc *target = PSNodeAlloc::get(ptr.target);
                        assert(target && "Target is not memory allocation");
                        if (!target->isHeap() && !target->isGlobal())
                            changed |= node->addPointsTo(INVALIDATED);
                    }
                }
            }
            // fall-through
        case PSNodeType::RETURN:
            // gather pointers returned from subprocedure - the same way
            // as PHI works
        case PSNodeType::PHI:
            for (PSNode *op : node->operands)
                changed |= node->addPointsTo(op->pointsTo);
            break;
        case PSNodeType::CALL_FUNCPTR:
            // call via function pointer:
            // first gather the pointers that can be used to the
            // call and if something changes, let backend take some action
            // (for example build relevant subgraph)
            for (const Pointer& ptr : node->getOperand(0)->pointsTo) {
                if (node->addPointsTo(ptr)) {
                    changed = true;

                    if (ptr.isValid() && !ptr.isInvalidated()) {
                        functionPointerCall(node, ptr.target);
                    } else {
                        error(node, "Calling invalid pointer as a function!");
                        continue;
                    }
                }
            }
            break;
        case PSNodeType::MEMCPY:
            changed |= processMemcpy(node);
            break;
        case PSNodeType::ALLOC:
        case PSNodeType::DYN_ALLOC:
        case PSNodeType::FUNCTION:
            // these two always points to itself
            assert(node->doesPointsTo(node, 0));
            assert(node->pointsTo.size() == 1);
        case PSNodeType::CALL:
        case PSNodeType::ENTRY:
        case PSNodeType::NOOP:
            // just no op
            break;
        default:
            assert(0 && "Unknown type");
    }

#ifdef DEBUG_ENABLED
    // the change of points-to set is not the only
    // change that can happen, so we don't use it as an
    // indicator and we use the 'changed' variable instead.
    // However, this assertion must hold:
    assert((node->pointsTo.size() == prev_size || changed)
           && "BUG: Did not set change but changed points-to sets");
#endif

    return changed;
}

} // namespace pta
} // namespace analysis
} // namespace dg
