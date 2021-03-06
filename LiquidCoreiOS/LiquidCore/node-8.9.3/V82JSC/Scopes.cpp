//
//  Scopes.cpp
//  LiquidCore
//
/*
 Copyright (c) 2018 Eric Lange. All rights reserved.
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 
 - Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 
 - Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "V82JSC.h"
#include "JSObjectRefPrivate.h"

using namespace v8;
#include <malloc/malloc.h>

#define HANDLEBLOCK_SIZE 0x4000
#define NUM_HANDLES ((HANDLEBLOCK_SIZE - 2*sizeof(HandleBlock*))/ sizeof(internal::Object*))
struct HandleBlock {
    HandleBlock * previous_;
    HandleBlock * next_;
    internal::Object * handles_[NUM_HANDLES];
};

static const uint8_t kActiveWeak = internal::Internals::kNodeStateIsWeakValue +
    (1 << internal::Internals::kNodeIsActiveShift);
static const uint8_t kActiveWeakMask = internal::Internals::kNodeStateMask +
    (1 << internal::Internals::kNodeIsActiveShift);
static const uint8_t kActiveNearDeath = internal::Internals::kNodeStateIsNearDeathValue +
    (1 << internal::Internals::kNodeIsActiveShift);

HandleScope::HandleScope(Isolate* isolate)
{
    IsolateImpl::PerThreadData::EnterThreadContext(isolate);
    Initialize(isolate);
}

void HandleScope::Initialize(Isolate* isolate)
{
    IsolateImpl* impl = V82JSC::ToIsolateImpl(isolate);
    
    isolate_ = reinterpret_cast<internal::Isolate*>(isolate);
    prev_next_ = impl->ii.handle_scope_data()->next;
    prev_limit_ = impl->ii.handle_scope_data()->limit;
    
    impl->m_scope_stack->push_back(this);
}


void delBlock(HandleBlock *block)
{
    if (block && block->next_) delBlock(block->next_);
    free (block);
}

HandleScope::~HandleScope()
{
    IsolateImpl* impl = reinterpret_cast<IsolateImpl*>(isolate_);
    internal::HandleScopeData *data = impl->ii.handle_scope_data();

    assert(this == impl->m_scope_stack->back());
    data->next = prev_next_;
    data->limit = prev_limit_;
    
    // Clear any HandleBlocks that are done
    if (data->limit) {
        intptr_t addr = reinterpret_cast<intptr_t>(data->limit - 1);
        addr &= ~(HANDLEBLOCK_SIZE-1);
        HandleBlock *thisBlock = reinterpret_cast<HandleBlock*>(addr);
        delBlock(thisBlock->next_);
        thisBlock->next_ = nullptr;
        if (impl->m_scope_stack->size() == 1) {
            CHECK_EQ(data->next, data->limit);
            free (thisBlock);
            data->next = nullptr;
            data->limit = nullptr;
        }
    }
    
    impl->m_scope_stack->pop_back();
}

//
// internal::HandleScope
//
// Deallocates any extensions used by the current scope.
void internal::HandleScope::DeleteExtensions(Isolate* isolate)
{
    assert(0);
}

// Counts the number of allocated handles.
int internal::HandleScope::NumberOfHandles(Isolate* ii)
{
    // Seal off current thread's handle scopes
    auto thread = IsolateImpl::PerThreadData::Get(reinterpret_cast<IsolateImpl*>(ii));
    memcpy(&thread->m_handle_scope_data, ii->handle_scope_data(), sizeof(HandleScopeData));

    size_t handles = 0;
    HandleScopeData *data = ii->handle_scope_data();
    if (data->limit) {
        intptr_t addr = reinterpret_cast<intptr_t>(data->limit - 1);
        addr &= ~(HANDLEBLOCK_SIZE -1);
        HandleBlock *block = reinterpret_cast<HandleBlock*>(addr);
        if (block) {
            handles = data->next - &block->handles_[0];
            block = block->previous_;
        }
        while (block) {
            handles += NUM_HANDLES;
            block = block->previous_;
        }
    }
    return (int)handles;
}

// Extend the handle scope making room for more handles.
internal::Object** internal::HandleScope::Extend(Isolate* isolate)
{
    IsolateImpl* isolateimpl = reinterpret_cast<IsolateImpl*>(isolate);
    DCHECK(!isolateimpl->m_scope_stack->empty());
    
    HandleBlock *ptr;
    posix_memalign((void**)&ptr, HANDLEBLOCK_SIZE, sizeof(HandleBlock));
    
    internal::Object **handles = &ptr->handles_[0];
    HandleScopeData *data = isolateimpl->ii.handle_scope_data();

    if (data->next != nullptr) {
        intptr_t addr = reinterpret_cast<intptr_t>(data->limit - 1);
        addr &= ~(HANDLEBLOCK_SIZE - 1);
        ptr->previous_ = reinterpret_cast<HandleBlock*>(addr);
        ptr->previous_->next_ = ptr;
    } else {
        ptr->previous_ = nullptr;
    }
    ptr->next_ = nullptr;

    data->next = handles;
    data->limit = &handles[NUM_HANDLES];

    return handles;
}

void IsolateImpl::GetActiveLocalHandles(std::map<v8::internal::Object*, int>& dontDeleteMap)
{
    // Seal off current thread's handle scopes
    auto thread = PerThreadData::Get(this);
    memcpy(&thread->m_handle_scope_data, ii.handle_scope_data(), sizeof(HandleScopeData));
    
    std::unique_lock<std::mutex> lock(s_thread_data_mutex);
    for (auto it=s_thread_data.begin(); it!=s_thread_data.end(); it++) {
        for (auto it2=it->second->m_isolate_data.begin(); it2!=it->second->m_isolate_data.end(); it2++) {
            if (it2->first == this) {
                HandleScopeData *data = &it2->second->m_handle_scope_data;
                if (data->limit) {
                    intptr_t addr = reinterpret_cast<intptr_t>(data->limit - 1);
                    addr &= ~(HANDLEBLOCK_SIZE -1);
                    HandleBlock *block = reinterpret_cast<HandleBlock*>(addr);
                    internal::Object ** limit = data->next;
                    while (block) {
                        for (internal::Object ** handle = &block->handles_[0]; handle < limit; handle++ ) {
                            if ((*handle)->IsHeapObject()) {
                                int count = dontDeleteMap.count(*handle) ? dontDeleteMap[*handle] : 0;
                                dontDeleteMap[*handle] = count + 1;
                            }
                        }
                        block = block->next_;
                        limit = block ? &block->next_->handles_[NUM_HANDLES] : nullptr;
                    }
                }
            }
        }
    }
}

internal::Object** internal::CanonicalHandleScope::Lookup(Object* object)
{
    assert(0);
    return nullptr;
}

int HandleScope::NumberOfHandles(Isolate* isolate)
{
    return internal::HandleScope::NumberOfHandles(reinterpret_cast<internal::Isolate*>(isolate));
}

internal::Object** HandleScope::CreateHandle(internal::Isolate* isolate,
                                       internal::Object* value)
{
    IsolateImpl* impl = V82JSC::ToIsolateImpl(reinterpret_cast<Isolate*>(isolate));
    return internal::HandleScope::CreateHandle(&impl->ii, value);
}

internal::Object** HandleScope::CreateHandle(internal::HeapObject* heap_object,
                                       internal::Object* value)
{
    return CreateHandle(heap_object->GetIsolate(), value);
}

EscapableHandleScope::EscapableHandleScope(Isolate* isolate) : HandleScope()
{
    IsolateImpl::PerThreadData::EnterThreadContext(isolate);
    // Creates a slot which we will transfer to the previous scope upon Escape()
    escape_slot_ = HandleScope::CreateHandle(reinterpret_cast<internal::Isolate*>(isolate), 0);
    Initialize(isolate);
}

internal::Object** EscapableHandleScope::Escape(internal::Object** escape_value)
{
    if (escape_value) {
        *escape_slot_ = *escape_value;
        return escape_slot_;
    } else {
        return nullptr;
    }
}

SealHandleScope::~SealHandleScope()
{
    
}

SealHandleScope::SealHandleScope(Isolate* isolate) : isolate_(nullptr)
{
    
}


class internal::GlobalHandles::Node {
public:
    internal::Object *handle_;
    uint16_t classId_; // internal::Internals::kNodeClassIdOffset (1 *kApiPointerSize)
    uint8_t  index_;
    uint8_t  flags_;   // internal::Internals::kNodeFlagsOffset (1 * kApiPointerSize + 3)
    uint8_t  reserved_[internal::kApiPointerSize - sizeof(uint32_t)]; // align
    WeakCallbackInfo<void>::Callback weak_callback_;
    WeakCallbackInfo<void>::Callback second_pass_callback_;
    void *param_;
    void *embedder_fields_[2];
    v8::WeakCallbackType type_;
};


class internal::GlobalHandles::NodeBlock {
public:
    void remove_used() {
        assert(next_block_ == nullptr);
        assert(prev_block_ == nullptr);
        assert(global_handles_->first_block_ != this);
        
        if (global_handles_->first_used_block_ == this) {
            assert(prev_used_block_ == nullptr);
            global_handles_->first_used_block_ = next_used_block_;
        }
        if (next_used_block_) {
            next_used_block_->prev_used_block_ = prev_used_block_;
        }
        if (prev_used_block_) {
            prev_used_block_->next_used_block_ = next_used_block_;
        }
        next_used_block_ = nullptr;
        prev_used_block_ = nullptr;
    }
    void push_used() {
        assert(next_block_ == nullptr);
        assert(prev_block_ == nullptr);
        assert(global_handles_->first_block_ != this);

        next_used_block_ = global_handles_->first_used_block_;
        prev_used_block_ = nullptr;
        if (next_used_block_) {
            next_used_block_->prev_used_block_ = this;
        }
        global_handles_->first_used_block_ = this;
    }
    void remove() {
        assert(next_used_block_ == nullptr);
        assert(prev_used_block_ == nullptr);
        assert(global_handles_->first_used_block_ != this);
        
        if (global_handles_->first_block_ == this) {
            assert(prev_block_ == nullptr);
            global_handles_->first_block_ = next_block_;
        }
        if (next_block_) {
            next_block_->prev_block_ = prev_block_;
        }
        if (prev_block_) {
            prev_block_->next_block_ = next_block_;
        }
        next_block_ = nullptr;
        prev_block_ = nullptr;
    }
    void push() {
        assert(next_used_block_ == nullptr);
        assert(prev_used_block_ == nullptr);
        assert(global_handles_->first_used_block_ != this);

        next_block_ = global_handles_->first_block_;
        prev_block_ = nullptr;
        if (next_block_) {
            next_block_->prev_block_ = this;
        }
        global_handles_->first_block_ = this;
    }
    NodeBlock(GlobalHandles *global_handles) {
        global_handles_ = global_handles;
        next_used_block_ = nullptr;
        prev_used_block_ = nullptr;
        next_block_ = nullptr;
        prev_block_ = nullptr;
        bitmap_ = 0xffffffffffffffff;
        push();
    }
    internal::Object ** New(internal::Object *value)
    {
        assert (bitmap_ != 0);
        int rightmost_set_bit_index = ffsll(bitmap_) - 1;
        uint64_t mask = (uint64_t)1 << rightmost_set_bit_index;
        bitmap_ &= ~mask;
        if (bitmap_ == 0) {
            // Remove from available pool and add to used list
            remove();
            push_used();
        }
        global_handles_->number_of_global_handles_ ++;
        memset(&handles_[rightmost_set_bit_index], 0, sizeof(Node));
        handles_[rightmost_set_bit_index].index_ = (int) rightmost_set_bit_index;
        handles_[rightmost_set_bit_index].handle_ = value;
        return &handles_[rightmost_set_bit_index].handle_;
    }
    void Reset(internal::Object ** handle)
    {
        if (malloc_size(this) == 0) return;
        
        int64_t index = (reinterpret_cast<intptr_t>(handle) - reinterpret_cast<intptr_t>(&handles_[0])) / sizeof(Node);
        assert(index >= 0 && index < 64);
        bool wasFull = bitmap_ == 0;
        uint64_t mask = (uint64_t)1 << index;
        assert((bitmap_ & mask) == 0);
        bitmap_ |= mask;
        global_handles_->number_of_global_handles_ --;
        if (bitmap_ == 0xffffffffffffffff) {
            // NodeBlock is empty, remove from available list and delete
            remove();
            delete this;
        } else if (wasFull) {
            // Remove from used list and add to avaialable pool
            remove_used();
            push();
        }
    }
    
private:
    NodeBlock *next_block_;
    NodeBlock *prev_block_;
    NodeBlock *next_used_block_;
    NodeBlock *prev_used_block_;
    GlobalHandles *global_handles_;
    uint64_t bitmap_;
    Node handles_[64];
    friend class internal::GlobalHandles;
    friend class IsolateImpl;
};

internal::GlobalHandles::GlobalHandles(internal::Isolate *isolate)
{
    isolate_ = isolate;
    number_of_global_handles_ = 0;
    first_block_ = nullptr;
    first_used_block_ = nullptr;
    first_free_ = nullptr;
    
    IsolateImpl *iso = reinterpret_cast<IsolateImpl*>(isolate);
    iso->getGlobalHandles = [&](std::map<internal::Object*, int>& canon,
                                std::map<internal::Object*, std::vector<internal::Object**>>& weak)
    {
        auto ProcessNodeBlock = [&](NodeBlock *block)
        {
            for (int i=0; i<64; i++) {
                uint64_t mask = (uint64_t)1 << i;
                if (~(block->bitmap_) & mask) {
                    Node& node = block->handles_[i];
                    internal::Object *h = node.handle_;
                    IsolateImpl* iso = reinterpret_cast<IsolateImpl*>(block->global_handles_->isolate());
                    if (h->IsHeapObject()) {
                        if ((node.flags_ & kActiveWeakMask) == internal::Internals::kNodeStateIsNearDeathValue ||
                            (node.flags_ & kActiveWeakMask) == internal::Internals::kNodeStateIsWeakValue) {

                            V82JSC_HeapObject::HeapObject *obj = V82JSC_HeapObject::FromHeapPointer(h);
                            V82JSC_HeapObject::BaseMap *map =
                                reinterpret_cast<V82JSC_HeapObject::BaseMap*>(V82JSC_HeapObject::FromHeapPointer(obj->m_map));
                            if (h->IsPrimitive() || (map != iso->m_value_map && map != iso->m_array_buffer_map)) {
                                if (weak.count(h)) weak[h].push_back(&node.handle_);
                                else {
                                    auto vector = std::vector<internal::Object**>();
                                    vector.push_back(&node.handle_);
                                    weak[h] = vector;
                                }
                            }
                        } else {
                            int count = canon.count(h) ? canon[h] : 0;
                            canon[h] = count + 1;
                        }
                    }
                }
            }
        };
        for (NodeBlock *block = first_used_block_; block; block = block->next_used_block_) {
            ProcessNodeBlock(block);
        }
        for (NodeBlock *block = first_block_; block; block = block->next_block_) {
            ProcessNodeBlock(block);
        }
    };
    
    // For all active, weak values that have not previously been marked for death but are currently
    // ready to die, save them from collection one last time so that the client has a chance to resurrect
    // them in callbacks before we make this final
    iso->performIncrementalMarking = [&](JSMarkerRef marker, std::map<void*, JSObjectRef>& ready_to_die)
    {
        auto ProcessNodeBlock = [&](NodeBlock *block)
        {
            for (int i=0; i<64; i++) {
                uint64_t mask = (uint64_t)1 << i;
                if (~(block->bitmap_) & mask) {
                    Node& node = block->handles_[i];
                    internal::Object *h = node.handle_;
                    if (h->IsHeapObject()) {
                        if ((node.flags_ & kActiveWeakMask) == kActiveWeak) {
                            ValueImpl *vi = V82JSC::ToImpl<ValueImpl>(&node.handle_);
                            if (!marker->IsMarked(marker, (JSObjectRef)vi->m_value)) {
                                node.flags_ = (node.flags_ & ~kActiveWeakMask) | kActiveNearDeath;
                                ready_to_die[&node] = (JSObjectRef) vi->m_value;
                            }
                        }
                    }
                }
            }
        };
        for (NodeBlock *block = first_used_block_; block; block = block->next_used_block_) {
            ProcessNodeBlock(block);
        }
        for (NodeBlock *block = first_block_; block; block = block->next_block_) {
            ProcessNodeBlock(block);
        }
        
        for (auto i=ready_to_die.begin(); i!=ready_to_die.end(); ++i) {
            ValueImpl *vi = V82JSC::ToImpl<ValueImpl>(&((Node*)i->first)->handle_);
            marker->Mark(marker, (JSObjectRef)vi->m_value);
        }
    };
    
    iso->weakObjectNearDeath = [&](internal::Object ** location,
                                   std::vector<v8::internal::SecondPassCallback>& callbacks,
                                   JSObjectRef object)
    {
        Node * node = reinterpret_cast<Node*>(location);
        int index = node->index_;
        intptr_t offset = reinterpret_cast<intptr_t>(&reinterpret_cast<NodeBlock*>(16)->handles_) - 16;
        intptr_t handle_array = reinterpret_cast<intptr_t>(location) - index * sizeof(Node);
        NodeBlock *block = reinterpret_cast<NodeBlock*>(handle_array - offset);

        assert((node->flags_ & kActiveWeakMask) == kActiveNearDeath ||
               (node->flags_ & kActiveWeakMask) == internal::Internals::kNodeStateIsWeakValue);
        SecondPassCallback second_pass;
        second_pass.callback_ = nullptr;
        second_pass.param_ = node->param_;
        second_pass.object_ = object;
        if (object != 0) {
            TrackedObjectImpl *wrap = getPrivateInstance(JSObjectGetGlobalContext(object), object);
            assert(wrap);
            second_pass.embedder_fields_[0] = wrap->m_embedder_data[0];
            second_pass.embedder_fields_[1] = wrap->m_embedder_data[1];
        }
        if (node->weak_callback_) {
            WeakCallbackInfo<void> info(reinterpret_cast<v8::Isolate*>(block->global_handles_->isolate()),
                                        node->param_,
                                        second_pass.embedder_fields_,
                                        &second_pass.callback_);
            node->weak_callback_(info);
        }
        if (second_pass.callback_) {
            callbacks.push_back(second_pass);
        }
    };

    iso->weakHeapObjectFinalized = [&](v8::Isolate *isolate, v8::internal::SecondPassCallback& callback)
    {
        assert(callback.callback_);
        WeakCallbackInfo<void> info(isolate,
                                    callback.param_,
                                    callback.embedder_fields_,
                                    nullptr);
        callback.callback_(info);
    };
    
    iso->weakJSObjectFinalized = [&](JSGlobalContextRef ctx, JSObjectRef obj)
    {
        IsolateImpl* iso;
        {
            std::unique_lock<std::mutex> lk(IsolateImpl::s_isolate_mutex);
            iso = IsolateImpl::s_context_to_isolate_map[ctx];
        }
        for (auto i=iso->m_second_pass_callbacks.begin(); i!=iso->m_second_pass_callbacks.end(); ++i) {
            if ((*i).object_ == obj) {
                (*i).ready_to_call = true;
            }
        }
    };
}

internal::Handle<internal::Object> internal::GlobalHandles::Create(Object* value)
{
    if (!first_block_) {
        new NodeBlock(this);
    }
    return Handle<Object>(first_block_->New(value));
}

void internal::GlobalHandles::Destroy(internal::Object** location)
{
    // Get NodeBlock from location
    Node * handle_loc = reinterpret_cast<Node*>(location);
    int index = handle_loc->index_;
    intptr_t offset = reinterpret_cast<intptr_t>(&reinterpret_cast<NodeBlock*>(16)->handles_) - 16;
    intptr_t handle_array = reinterpret_cast<intptr_t>(location) - index * sizeof(Node);
    NodeBlock *block = reinterpret_cast<NodeBlock*>(handle_array - offset);
    block->Reset(location);
}

internal::Handle<internal::Object> internal::GlobalHandles::CopyGlobal(v8::internal::Object **location)
{
    Node * handle_loc = reinterpret_cast<Node*>(location);
    int index = handle_loc->index_;
    intptr_t offset = reinterpret_cast<intptr_t>(&reinterpret_cast<NodeBlock*>(16)->handles_) - 16;
    intptr_t handle_array = reinterpret_cast<intptr_t>(location) - index * sizeof(Node);
    NodeBlock *block = reinterpret_cast<NodeBlock*>(handle_array - offset);
    
    Handle<Object> new_handle = block->global_handles_->Create(*location);
    // Copy traits (preserve index of new handle)
    internal::GlobalHandles::Node * new_handle_loc = reinterpret_cast<internal::GlobalHandles::Node*>(new_handle.location());
    index = new_handle_loc->index_;
    memcpy(new_handle_loc, handle_loc, sizeof(Node));
    new_handle_loc->index_ = index;
    
    return new_handle;
}

void internal::GlobalHandles::MakeWeak(internal::Object **location, void *parameter,
                                       WeakCallbackInfo<void>::Callback weak_callback, v8::WeakCallbackType type)
{
    Node * handle_loc = reinterpret_cast<Node*>(location);
    int index = handle_loc->index_;
    intptr_t offset = reinterpret_cast<intptr_t>(&reinterpret_cast<NodeBlock*>(16)->handles_) - 16;
    intptr_t handle_array = reinterpret_cast<intptr_t>(location) - index * sizeof(Node);
    NodeBlock *block = reinterpret_cast<NodeBlock*>(handle_array - offset);
    v8::Isolate *isolate = reinterpret_cast<v8::Isolate*>(block->global_handles_->isolate());
    IsolateImpl *iso = V82JSC::ToIsolateImpl(isolate);
    
    v8::HandleScope scope(isolate);
    Local<v8::Context> context = V82JSC::OperatingContext(isolate);
    JSContextRef ctx = V82JSC::ToContextRef(context);
    if ((*location)->IsHeapObject()) {
        V82JSC_HeapObject::HeapObject *obj = V82JSC_HeapObject::FromHeapPointer(*location);
        V82JSC_HeapObject::BaseMap *map =
            reinterpret_cast<V82JSC_HeapObject::BaseMap*>(V82JSC_HeapObject::FromHeapPointer(obj->m_map));
        if (!(*location)->IsPrimitive() && (map == iso->m_value_map || map == iso->m_array_buffer_map)) {
            ValueImpl *value = static_cast<ValueImpl*>(obj);
            if (JSValueIsObject(ctx, value->m_value)) {
                makePrivateInstance(iso, ctx, (JSObjectRef)value->m_value);
                handle_loc->flags_ |= (1 << internal::Internals::kNodeIsActiveShift);
                V82JSC_HeapObject::WeakValue* weak =
                    static_cast<V82JSC_HeapObject::WeakValue*>
                    (V82JSC_HeapObject::HeapAllocator::Alloc(iso, iso->m_weak_value_map));
                weak->m_value = value->m_value;
                handle_loc->handle_ = V82JSC_HeapObject::ToHeapPointer(weak);
            }
        }
    }

    handle_loc->flags_ = (handle_loc->flags_ & ~internal::Internals::kNodeStateMask) |
        internal::Internals::kNodeStateIsWeakValue;
    handle_loc->param_ = parameter;
    handle_loc->type_ = type;
    handle_loc->weak_callback_ = weak_callback;
    handle_loc->second_pass_callback_ = nullptr;
}
void internal::GlobalHandles::MakeWeak(v8::internal::Object ***location_addr)
{
    MakeWeak(*location_addr, nullptr, nullptr, WeakCallbackType::kParameter);
}

void * internal::GlobalHandles::ClearWeakness(v8::internal::Object **location)
{
    Node * handle_loc = reinterpret_cast<Node*>(location);
    if ((handle_loc->flags_ & kActiveWeakMask) != kActiveWeak) return nullptr;

    int index = handle_loc->index_;
    intptr_t offset = reinterpret_cast<intptr_t>(&reinterpret_cast<NodeBlock*>(16)->handles_) - 16;
    intptr_t handle_array = reinterpret_cast<intptr_t>(location) - index * sizeof(Node);
    NodeBlock *block = reinterpret_cast<NodeBlock*>(handle_array - offset);
    v8::Isolate *isolate = reinterpret_cast<v8::Isolate*>(block->global_handles_->isolate());
    IsolateImpl *iso = V82JSC::ToIsolateImpl(isolate);

    v8::HandleScope scope(isolate);
    Local<v8::Context> context = V82JSC::OperatingContext(isolate);
    JSContextRef ctx = V82JSC::ToContextRef(context);
    if ((*location)->IsHeapObject()) {
        V82JSC_HeapObject::HeapObject *obj = V82JSC_HeapObject::FromHeapPointer(*location);
        if ((V82JSC_HeapObject::BaseMap*)V82JSC_HeapObject::FromHeapPointer(obj->m_map) == iso->m_weak_value_map) {
            V82JSC_HeapObject::WeakValue *value = static_cast<V82JSC_HeapObject::WeakValue*>(obj);
            ValueImpl* strong = static_cast<ValueImpl*>
                (V82JSC_HeapObject::HeapAllocator::Alloc(iso, iso->m_value_map));
            strong->m_value = value->m_value;
            JSValueProtect(ctx, strong->m_value);
            handle_loc->handle_ = V82JSC_HeapObject::ToHeapPointer(strong);
        }
    }

    handle_loc->flags_ &= ~kActiveWeakMask;
    void *param = handle_loc->param_;
    handle_loc->param_ = nullptr;
    handle_loc->weak_callback_ = nullptr;
    handle_loc->second_pass_callback_ = nullptr;
    
    return param;
}

void internal::GlobalHandles::TearDown()
{
    for (NodeBlock *block = first_block_; block; ) {
        NodeBlock *next = block->next_block_;
        delete block;
        block = next;
    }
    for (NodeBlock *block = first_used_block_; block; ) {
        NodeBlock *next = block->next_used_block_;
        delete block;
        block = next;
    }
}

void V8::MakeWeak(internal::Object** location, void* data,
                  WeakCallbackInfo<void>::Callback weak_callback,
                  WeakCallbackType type)
{
    internal::GlobalHandles::MakeWeak(location, data, weak_callback, type);
}
void V8::MakeWeak(internal::Object** location, void* data,
                  // Must be 0 or -1.
                  int internal_field_index1,
                  // Must be 1 or -1.
                  int internal_field_index2,
                  WeakCallbackInfo<void>::Callback weak_callback)
{
    // FIXME:  Is this even being used?  If not, get rid of it.
    assert(0);
}
void V8::MakeWeak(internal::Object*** location_addr)
{
    internal::GlobalHandles::MakeWeak(location_addr);
}
void* V8::ClearWeak(internal::Object** location)
{
    return internal::GlobalHandles::ClearWeakness(location);
}

internal::Object** V8::GlobalizeReference(internal::Isolate* isolate,
                                          internal::Object** handle)
{
    return isolate->global_handles()->Create(*handle).location();
}
internal::Object** V8::CopyPersistent(internal::Object** handle)
{
    return internal::GlobalHandles::CopyGlobal(handle).location();
}
void V8::DisposeGlobal(internal::Object** global_handle)
{
    internal::GlobalHandles::Destroy(global_handle);
}
