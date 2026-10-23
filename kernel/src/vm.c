#include "vm.h"
#include "exec.h"

#include <limits.h>
#include <string.h>

#define VM_DEFAULT_VALUES 65536u
#define VM_DEFAULT_FRAMES 2048u
#define VM_DEFAULT_DYNAMICS 2048u
#define VM_DEFAULT_EMERGENCY_VALUES 1024u
#define VM_DEFAULT_EMERGENCY_FRAMES 64u
#define VM_DEFAULT_EMERGENCY_BYTES (64u * 1024u)
#define VM_PROTO_SYMBOL_FLAGS P_EVAL_COMPILED

typedef struct {
    size_t pc, locals_base, operand_base, return_sp, dynamic_base;
} VMFrame;

typedef enum {
    VM_DYN_SPECIAL, VM_DYN_CATCH, VM_DYN_BLOCK, VM_DYN_TAGBODY, VM_DYN_UNWIND
} VMDynamicKind;

typedef struct {
    VMDynamicKind kind;
    size_t owner_frame, target_pc, stack_height, label_count;
} VMDynamic;

typedef struct {
    size_t value_capacity, frame_capacity, dynamic_capacity;
    size_t sp, frame_count, dynamic_count;
    size_t values_offset, frames_offset, closure_roots_offset;
    size_t dynamics_offset, dynamic_roots_offset;
    bool unwind_active, unwind_waiting;
    size_t unwind_depth, unwind_wait_frame, unwind_wait_sp, unwind_action;
    size_t transfer_owner, transfer_pc, transfer_stack, transfer_kind;
    size_t transfer_label, transfer_dynamic_index;
} VMCore;

enum { VM_UNWIND_NONE, VM_UNWIND_POP, VM_UNWIND_RETURN,
       VM_UNWIND_TRANSFER, VM_UNWIND_ERROR };

static size_t align_size(size_t value, size_t alignment)
{
    if (value > SIZE_MAX - (alignment - 1u)) return 0u;
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static bool layout_bytes(size_t values, size_t frames, size_t dynamics,
                         size_t *total, VMCore *layout)
{
    /* Every scalar VM descriptor is word-aligned; sizeof(Value) is a legal
     * C99 alignment boundary and, unlike sizeof(VMFrame), is a power of two. */
    size_t at = align_size(sizeof(VMCore), sizeof(Value));
    if (at == 0u || values > (SIZE_MAX - at) / sizeof(Value)) return false;
    layout->values_offset = at;
    at += values * sizeof(Value);
    at = align_size(at, sizeof(Value));
    if (at == 0u || frames > (SIZE_MAX - at) / sizeof(VMFrame)) return false;
    layout->frames_offset = at;
    at += frames * sizeof(VMFrame);
    at = align_size(at, sizeof(Value));
    if (at == 0u || frames > (SIZE_MAX - at) / sizeof(Value)) return false;
    layout->closure_roots_offset = at;
    at += frames * sizeof(Value);
    at = align_size(at, sizeof(Value));
    if (at == 0u || dynamics > (SIZE_MAX - at) / sizeof(VMDynamic)) return false;
    layout->dynamics_offset = at;
    at += dynamics * sizeof(VMDynamic);
    at = align_size(at, sizeof(Value));
    if (at == 0u || dynamics > (SIZE_MAX - at) / (3u * sizeof(Value))) return false;
    layout->dynamic_roots_offset = at;
    at += dynamics * 3u * sizeof(Value);
    *total = at;
    layout->value_capacity = values;
    layout->frame_capacity = frames;
    layout->dynamic_capacity = dynamics;
    return true;
}

static VMCore *vm_core(PVM *vm)
{
    return vm == NULL || vm->owner == 0u ? NULL :
        (VMCore *)((PBytes *)vm->owner)->data;
}

static void vm_sync_value_roots(PVM *vm, VMCore *core)
{
    if (vm != NULL && vm->state != NULL &&
        vm->value_root_index < vm->state->p->root_count)
        vm->state->p->roots[vm->value_root_index].count = core->sp;
}

static Value *vm_values(PVM *vm, VMCore *core)
{
    (void)vm;
    return (Value *)((uint8_t *)core + core->values_offset);
}

static VMFrame *vm_frames(PVM *vm, VMCore *core)
{
    (void)vm;
    return (VMFrame *)((uint8_t *)core + core->frames_offset);
}

static Value *vm_closure_roots(PVM *vm, VMCore *core)
{
    (void)vm;
    return (Value *)((uint8_t *)core + core->closure_roots_offset);
}

static VMDynamic *vm_dynamics(PVM *vm, VMCore *core)
{
    (void)vm;
    return (VMDynamic *)((uint8_t *)core + core->dynamics_offset);
}

static Value *vm_dynamic_roots(PVM *vm, VMCore *core)
{
    (void)vm;
    return (Value *)((uint8_t *)core + core->dynamic_roots_offset);
}

static PBytes *vm_bytes(Value value)
{
    return (PBytes *)value;
}

void p_vm_default_config(PVMConfig *config)
{
    if (config == NULL) return;
    config->value_capacity = VM_DEFAULT_VALUES;
    config->frame_capacity = VM_DEFAULT_FRAMES;
    config->dynamic_capacity = VM_DEFAULT_DYNAMICS;
    config->emergency_values = VM_DEFAULT_EMERGENCY_VALUES;
    config->emergency_frames = VM_DEFAULT_EMERGENCY_FRAMES;
    config->emergency_bytes = VM_DEFAULT_EMERGENCY_BYTES;
}

bool p_vm_init(PVM *vm, PState *state, const PVMConfig *requested)
{
    if (vm == NULL || state == NULL || !state->initialized || vm->initialized)
        return false;
    memset(vm, 0, sizeof(*vm));
    PVMConfig config;
    if (requested == NULL) p_vm_default_config(&config);
    else config = *requested;
    if (config.value_capacity > SIZE_MAX - config.emergency_values ||
        config.frame_capacity > SIZE_MAX - config.emergency_frames ||
        config.dynamic_capacity > SIZE_MAX - config.emergency_frames ||
        config.value_capacity + config.emergency_values == 0u ||
        config.frame_capacity + config.emergency_frames == 0u ||
        config.dynamic_capacity == 0u) {
        (void)p_error(state->p, P_ERR_RANGE, "invalid VM capacity configuration");
        return false;
    }
    size_t values = config.value_capacity + config.emergency_values;
    size_t frames = config.frame_capacity + config.emergency_frames;
    size_t dynamics = config.dynamic_capacity + config.emergency_frames;
    size_t bytes;
    VMCore layout;
    memset(&layout, 0, sizeof(layout));
    if (!layout_bytes(values, frames, dynamics, &bytes, &layout) ||
        bytes > SIZE_MAX - offsetof(PBytes, data)) {
        (void)p_error(state->p, P_ERR_RANGE, "VM arena layout overflows size_t");
        return false;
    }
    size_t saved = p_root_save(state->p);
    if (!p_roots(state->p, &vm->owner, 1u) ||
        !p_reserve(state->p, offsetof(PBytes, data) + bytes)) {
        p_root_restore(state->p, saved);
        return false;
    }
    vm->owner = p_bytes(state->p, bytes);
    if (vm->owner == 0u) {
        p_root_restore(state->p, saved);
        return false;
    }
    VMCore *core = vm_core(vm);
    memcpy(core, &layout, sizeof(layout));
    Value *values_root = vm_values(vm, core);
    Value *closure_root = vm_closure_roots(vm, core);
    Value *dynamic_root = vm_dynamic_roots(vm, core);
    memset(values_root, 0, values * sizeof(Value));
    memset(closure_root, 0, frames * sizeof(Value));
    memset(dynamic_root, 0, dynamics * 3u * sizeof(Value));
    vm->value_root_index = state->p->root_count;
    if (!p_roots(state->p, values_root, values) ||
        !p_roots(state->p, closure_root, frames) ||
        !p_roots(state->p, dynamic_root, dynamics * 3u) ||
        !p_roots(state->p, vm->unwind_values,
                 P_MULTIPLE_VALUES_LIMIT + 2u) ||
        !p_roots(state->p, &vm->emergency_heap_reserve, 1u)) {
        p_root_restore(state->p, saved);
        vm->owner = 0u;
        return false;
    }
    if (config.emergency_bytes != 0u) {
        if (config.emergency_bytes > SIZE_MAX - offsetof(PBytes, data) ||
            !p_reserve(state->p, offsetof(PBytes, data) + config.emergency_bytes)) {
            p_root_restore(state->p, saved);
            vm->owner = 0u;
            return false;
        }
        vm->emergency_heap_reserve = p_bytes(state->p, config.emergency_bytes);
        if (vm->emergency_heap_reserve == 0u) {
            p_root_restore(state->p, saved);
            vm->owner = 0u;
            return false;
        }
    }
    vm->saved_root_count = saved;
    vm->state = state;
    vm->implementation = core;
    vm->config = config;
    vm->initialized = true;
    vm_sync_value_roots(vm, core);
    state->vm_engine = vm;
    return true;
}

void p_vm_destroy(PVM *vm)
{
    if (vm == NULL || !vm->initialized || vm->state == NULL) return;
    PState *state = vm->state;
    if (state->vm_engine == vm) state->vm_engine = NULL;
    p_root_restore(state->p, vm->saved_root_count);
    memset(vm, 0, sizeof(*vm));
}

static bool fixnum_field(Pocket *p, Value value, size_t *number)
{
    if (!p_is_fixnum(value)) {
        (void)p_error(p, P_ERR_TYPE, "proto metadata field must be a fixnum");
        return false;
    }
    int64_t signed_number = p_fixnum_value(value);
    if (signed_number < 0 || (uint64_t)signed_number > SIZE_MAX) {
        (void)p_error(p, P_ERR_RANGE, "proto metadata field is out of range");
        return false;
    }
    *number = (size_t)signed_number;
    return true;
}

Value p_vm_make_proto(Pocket *p, const Value fields[P_PROTO_FIELD_COUNT])
{
    if (p == NULL || fields == NULL || p_type(fields[P_PROTO_CODE]) != P_BYTES ||
        p_type(fields[P_PROTO_CONSTANTS]) != P_VECTOR) {
        if (p != NULL) (void)p_error(p, P_ERR_TYPE, "proto requires byte code and a constants vector");
        return 0u;
    }
    PProto proto;
    size_t target_bits;
    memset(&proto, 0, sizeof(proto));
    proto.name = fields[P_PROTO_NAME];
    proto.rest = fields[P_PROTO_REST] != p->nil;
    if (!fixnum_field(p, fields[P_PROTO_REQUIRED], &proto.required) ||
        !fixnum_field(p, fields[P_PROTO_LOCALS], &proto.locals) ||
        !fixnum_field(p, fields[P_PROTO_FREE], &proto.free_count) ||
        !fixnum_field(p, fields[P_PROTO_STACK], &proto.max_stack) ||
        !fixnum_field(p, fields[P_PROTO_TARGET_BITS], &target_bits) ||
        target_bits > UINT_MAX)
        return 0u;
    proto.target_bits = (unsigned)target_bits;
    proto.code = fields[P_PROTO_CODE];
    proto.constants = fields[P_PROTO_CONSTANTS];
    proto.flags = 0u;
    if (proto.required > proto.locals ||
        (proto.rest && proto.required >= proto.locals)) {
        (void)p_error(p, P_ERR_RANGE, "proto local count does not fit its parameters");
        return 0u;
    }
    if (p_fixnum_value(fields[P_PROTO_TARGET_BITS]) != (int64_t)p_fixnum_bits()) {
        (void)p_error(p, P_ERR_IMAGE, "proto targets a different fixnum width");
        return 0u;
    }
    PProto *result = (PProto *)p_alloc(p, P_PROTO, sizeof(PProto));
    if (result == NULL) return 0u;
    result->name = proto.name;
    result->code = proto.code;
    result->constants = proto.constants;
    result->required = proto.required;
    result->locals = proto.locals;
    result->free_count = proto.free_count;
    result->max_stack = proto.max_stack;
    result->target_bits = proto.target_bits;
    result->flags = proto.flags;
    result->rest = proto.rest;
    return (Value)result;
}

Value p_vm_make_closure(Pocket *p, Value proto_value, Value captures)
{
    if (p == NULL || p_type(proto_value) != P_PROTO || p_type(captures) != P_VECTOR) {
        if (p != NULL) (void)p_error(p, P_ERR_TYPE, "compiled closure requires a proto and capture vector");
        return 0u;
    }
    PProto *proto = (PProto *)proto_value;
    if ((proto->flags & P_PROTO_VERIFIED) == 0u ||
        p_vector_ptr(captures)->length < proto->free_count) {
        (void)p_error(p, P_ERR_IMAGE, "compiled closure has unverified code or too few captures");
        return 0u;
    }
    PClosure *closure = (PClosure *)p_alloc(p, P_CLOSURE, sizeof(PClosure));
    if (closure == NULL) return 0u;
    closure->name = proto->name;
    closure->parameters = p->nil;
    closure->body = proto_value;
    closure->environment = captures;
    closure->required_count = proto->required;
    closure->rest_parameter = p->nil;
    closure->flags = VM_PROTO_SYMBOL_FLAGS;
    return (Value)closure;
}

static bool opcode_operand_width(uint8_t opcode, size_t *width)
{
    switch ((POpcode)opcode) {
    case P_OP_CONST: case P_OP_GLOBAL: case P_OP_SET_GLOBAL: case P_OP_FUNCTION:
    case P_OP_LOCAL: case P_OP_SET_LOCAL: case P_OP_FREE: case P_OP_JUMP:
    case P_OP_JUMP_NIL: case P_OP_CALL: case P_OP_TAIL_CALL: case P_OP_CLOSURE:
    case P_OP_VALUES: case P_OP_BIND_SPECIAL: case P_OP_PUSH_CATCH:
    case P_OP_PUSH_BLOCK: case P_OP_PUSH_TAGBODY: case P_OP_POP_DYNAMIC:
    case P_OP_GO: case P_OP_ADD: case P_OP_SUB: case P_OP_LT:
    case P_OP_NUM_EQ: case P_OP_CONS: case P_OP_CAR: case P_OP_CDR:
        *width = 4u; return true;
    case P_OP_POP: case P_OP_DUP: case P_OP_RETURN: case P_OP_BOX:
    case P_OP_UNBOX: case P_OP_SET_BOX: case P_OP_MV_LIST: case P_OP_MV_RESTORE:
    case P_OP_PUSH_UNWIND: case P_OP_THROW: case P_OP_RETURN_FROM:
        *width = 0u; return true;
    default: return false;
    }
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool verify_constant(Pocket *p, PProto *proto, uint32_t index,
                            bool symbol_only)
{
    if (p_type(proto->constants) != P_VECTOR) return false;
    PVector *constants = p_vector_ptr(proto->constants);
    if ((size_t)index >= constants->length) return false;
    Value value = constants->data[index];
    if (!symbol_only) return true;
    if (p_type(value) != P_SYMBOL) {
        (void)p_error(p, P_ERR_IMAGE, "VM symbol operand does not name a symbol constant");
        return false;
    }
    return true;
}

bool p_vm_verify_workspace(size_t code_length, size_t *bytes)
{
    if (bytes == NULL || code_length == SIZE_MAX) return false;
    size_t entries = code_length + 1u;
    if (entries > (SIZE_MAX - (sizeof(size_t) - 1u)) /
                  (1u + 2u * sizeof(size_t))) return false;
    *bytes = entries + (sizeof(size_t) - 1u) +
             entries * 2u * sizeof(size_t);
    return true;
}

static size_t *workspace_arrays(void *workspace, size_t workspace_size,
                               size_t entries, uint8_t **boundaries)
{
    if (workspace == NULL) return NULL;
    uintptr_t base = (uintptr_t)workspace;
    uintptr_t depth_address = (base + entries + sizeof(size_t) - 1u) &
                              ~(uintptr_t)(sizeof(size_t) - 1u);
    size_t offset = (size_t)(depth_address - base);
    if (offset > workspace_size || entries >
        (workspace_size - offset) / (2u * sizeof(size_t))) return NULL;
    *boundaries = (uint8_t *)workspace;
    return (size_t *)depth_address;
}

static bool vm_fail(PVM *vm, PErrorCode code, const char *message)
{
    (void)p_error(vm->state->p, code, message);
    return false;
}

static bool vm_push(PVM *vm, VMCore *core, Value value)
{
    size_t limit = (vm->emergency_mode || core->unwind_active) ?
        core->value_capacity : vm->config.value_capacity;
    if (core->sp >= limit) return vm_fail(vm, P_ERR_STACK, "VM value stack exhausted");
    vm_values(vm, core)[core->sp++] = value;
    return true;
}

static Value vm_pop(PVM *vm, VMCore *core)
{
    if (core->sp == 0u) {
        (void)vm_fail(vm, P_ERR_IMAGE, "verified VM code underflowed its stack");
        return 0u;
    }
    return vm_values(vm, core)[--core->sp];
}

static bool vm_dynamic_push(PVM *vm, VMCore *core, VMDynamicKind kind,
                            Value first, Value second, Value third,
                            size_t target, size_t stack_height)
{
    size_t limit = (vm->emergency_mode || core->unwind_active) ?
        core->dynamic_capacity : vm->config.dynamic_capacity;
    if (core->dynamic_count >= limit)
        return vm_fail(vm, P_ERR_STACK, "VM dynamic stack exhausted");
    size_t index = core->dynamic_count++;
    VMDynamic *entry = &vm_dynamics(vm, core)[index];
    entry->kind = kind;
    entry->owner_frame = core->frame_count == 0u ? 0u : core->frame_count - 1u;
    entry->target_pc = target;
    entry->stack_height = stack_height;
    entry->label_count = 0u;
    Value *roots = vm_dynamic_roots(vm, core) + index * 3u;
    roots[0] = first; roots[1] = second; roots[2] = third;
    return true;
}

static bool vm_dispatch_call(PVM *vm, VMCore *core, size_t base,
                             size_t argc, bool tail, size_t baseline);
static bool vm_fail(PVM *vm, PErrorCode code, const char *message);
static bool vm_return(PVM *vm, VMCore *core, Value result);
static bool vm_continue_unwind(PVM *vm, VMCore *core);
static bool vm_apply_transfer(PVM *vm, VMCore *core);

static void vm_save_unwind_values(PVM *vm)
{
    size_t count = vm->state->mv_count;
    if (count > P_MULTIPLE_VALUES_LIMIT) count = P_MULTIPLE_VALUES_LIMIT;
    memcpy(vm->unwind_values, vm->state->mv, count * sizeof(Value));
    vm->unwind_value_count = count;
}

static void vm_restore_unwind_values(PVM *vm)
{
    memcpy(vm->state->mv, vm->unwind_values,
           vm->unwind_value_count * sizeof(Value));
    vm->state->mv_count = vm->unwind_value_count;
}

static bool vm_restore_dynamics(PVM *vm, VMCore *core, size_t depth)
{
    if (depth > core->dynamic_count)
        return vm_fail(vm, P_ERR_IMAGE, "VM dynamic depth moved past its target");
    /* A cleanup frame's own RETURN reaches this path with its current dynamic
     * depth. That must leave the enclosing unwind continuation untouched. */
    if (core->unwind_active && depth >= core->dynamic_count) return true;
    if (!core->unwind_active) {
        core->unwind_active = true;
        core->unwind_waiting = false;
        core->unwind_depth = depth;
        if (core->unwind_action == VM_UNWIND_NONE)
            core->unwind_action = VM_UNWIND_POP;
        vm_save_unwind_values(vm);
    } else if (depth != core->unwind_depth) {
        return vm_fail(vm, P_ERR_IMAGE, "nested dynamic unwind lacks a VM continuation");
    }
    return vm_continue_unwind(vm, core);
}

static bool vm_continue_unwind(PVM *vm, VMCore *core)
{
    Pocket *p = vm->state->p;
    VMDynamic *entries = vm_dynamics(vm, core);
    Value *roots = vm_dynamic_roots(vm, core);
    if (core->unwind_waiting) {
        if (core->frame_count > core->unwind_wait_frame) return true;
        core->unwind_waiting = false;
        core->sp = core->unwind_wait_sp;
        vm_restore_unwind_values(vm);
    }
    while (core->dynamic_count > core->unwind_depth) {
        size_t index = --core->dynamic_count;
        VMDynamic *entry = &entries[index];
        if (entry->kind == VM_DYN_SPECIAL && p_type(roots[index * 3u]) == P_SYMBOL) {
            p_symbol_ptr(roots[index * 3u])->value = roots[index * 3u + 1u];
        } else if (entry->kind == VM_DYN_UNWIND) {
            Value cleanup = roots[index * 3u];
            roots[index * 3u] = roots[index * 3u + 1u] = roots[index * 3u + 2u] = 0u;
            size_t base = core->sp;
            size_t frames_before = core->frame_count;
            if (!vm_push(vm, core, cleanup) ||
                !vm_dispatch_call(vm, core, base, 0u, false, frames_before))
                return false;
            if (core->frame_count > frames_before) {
                core->unwind_waiting = true;
                core->unwind_wait_frame = frames_before;
                core->unwind_wait_sp = base;
                return true;
            }
            core->sp = base;
            vm_restore_unwind_values(vm);
            if (p->error.code != P_OK) return false;
        }
        roots[index * 3u] = roots[index * 3u + 1u] = roots[index * 3u + 2u] = 0u;
    }

    size_t action = core->unwind_action;
    core->unwind_active = false;
    core->unwind_waiting = false;
    core->unwind_action = VM_UNWIND_NONE;
    vm_restore_unwind_values(vm);
    if (action == VM_UNWIND_RETURN) {
        Value result = vm->unwind_values[P_MULTIPLE_VALUES_LIMIT];
        return vm_return(vm, core, result);
    }
    if (action == VM_UNWIND_TRANSFER) return vm_apply_transfer(vm, core);
    if (action == VM_UNWIND_ERROR) {
        vm->state->p->error = vm->pending_error;
        p_root_restore(vm->state->p, vm->error_root_base);
        vm->signaling_error = false;
        vm->error_hook_completed = false;
        return false;
    }
    return p->error.code == P_OK;
}

static bool vm_transfer(PVM *vm, VMCore *core, VMDynamicKind kind,
                        Value key, size_t label)
{
    VMDynamic *entries = vm_dynamics(vm, core);
    Value *roots = vm_dynamic_roots(vm, core);
    size_t target = core->dynamic_count;
    while (target != 0u) {
        --target;
        VMDynamic *entry = &entries[target];
        bool match = entry->kind == kind && roots[target * 3u] == key;
        if (match && kind == VM_DYN_TAGBODY) {
            PVector *labels = p_vector_ptr(roots[target * 3u + 1u]);
            if (label >= labels->length || !p_is_fixnum(labels->data[label]) ||
                p_fixnum_value(labels->data[label]) < 0) {
                return vm_fail(vm, P_ERR_CONTROL, "GO target is not in the active TAGBODY");
            }
        }
        if (match) break;
    }
    if (target == 0u &&
        !(core->dynamic_count != 0u && entries[0].kind == kind &&
          roots[0] == key)) {
        return vm_fail(vm, P_ERR_CONTROL, "no active target for VM control transfer");
    }

    size_t owner = entries[target].owner_frame;
    if (owner >= core->frame_count)
        return vm_fail(vm, P_ERR_IMAGE, "control target refers to a dead VM frame");
    size_t target_depth = target + 1u;
    if (core->unwind_active && core->unwind_action == VM_UNWIND_ERROR) {
        p_root_restore(vm->state->p, vm->error_root_base);
        memset(&vm->pending_error, 0, sizeof(vm->pending_error));
        vm->error_values[0] = vm->error_values[1] = 0u;
    }
    core->unwind_active = true;
    core->unwind_waiting = false;
    core->unwind_depth = target_depth;
    core->unwind_action = VM_UNWIND_TRANSFER;
    core->transfer_owner = owner;
    core->transfer_kind = (size_t)kind;
    core->transfer_label = label;
    core->transfer_dynamic_index = target;
    vm_save_unwind_values(vm);
    return vm_continue_unwind(vm, core);
}

static bool vm_apply_transfer(PVM *vm, VMCore *core)
{
    Pocket *p = vm->state->p;
    VMDynamic *entries = vm_dynamics(vm, core);
    Value *roots = vm_dynamic_roots(vm, core);
    size_t target = core->transfer_dynamic_index;
    VMDynamicKind kind = (VMDynamicKind)core->transfer_kind;
    size_t label = core->transfer_label;
    size_t owner = core->transfer_owner;
    if (target >= core->dynamic_count || owner >= core->frame_count)
        return vm_fail(vm, P_ERR_IMAGE, "unwind continuation lost its transfer target");
    /* The target's dynamic extent ends when CATCH/ BLOCK is exited. TAGBODY
     * remains active so another GO to one of its labels remains valid. */
    size_t target_pc = entries[target].target_pc;
    size_t stack_height = entries[target].stack_height;
    if (kind != VM_DYN_TAGBODY) {
        --core->dynamic_count;
        roots[target * 3u] = roots[target * 3u + 1u] =
            roots[target * 3u + 2u] = 0u;
    }
    while (core->frame_count > owner + 1u) {
        size_t index = --core->frame_count;
        vm_closure_roots(vm, core)[index] = 0u;
    }
    if (vm->signaling_error && owner < vm->error_hook_frame) {
        p_root_restore(p, vm->error_root_base);
        vm->signaling_error = false;
        vm->error_hook_completed = false;
        memset(&vm->pending_error, 0, sizeof(vm->pending_error));
    }
    VMFrame *frame = &vm_frames(vm, core)[owner];
    if (kind == VM_DYN_TAGBODY) {
        PVector *labels = p_vector_ptr(roots[target * 3u + 1u]);
        frame->pc = (size_t)p_fixnum_value(labels->data[label]);
        /* PUSHTAGBODY's token is consumed by the compiler's local before
         * entering the body.  The recorded height is the operand baseline
         * expected at every label; GO must not put that token back. */
        core->sp = stack_height;
        return true;
    }
    frame->pc = target_pc;
    core->sp = stack_height + 1u;
    vm_values(vm, core)[stack_height] =
        vm->state->mv_count == 0u ? p->nil : vm->state->mv[0];
    return true;
}

static bool vm_enter_compiled(PVM *vm, VMCore *core, Value function,
                              size_t call_base, size_t argc, bool tail)
{
    Pocket *p = vm->state->p;
    PClosure *closure = (PClosure *)function;
    PProto *proto = (PProto *)closure->body;
    if ((closure->flags & P_EVAL_COMPILED) == 0u || p_type(closure->body) != P_PROTO ||
        (proto->flags & P_PROTO_VERIFIED) == 0u)
        return vm_fail(vm, P_ERR_IMAGE, "compiled closure has no verified proto");
    if (argc < proto->required || (!proto->rest && argc != proto->required)) {
        (void)p_error(p, P_ERR_ARITY, "compiled closure received the wrong number of arguments");
        p->error.datum = closure->name;
        p->error.expected = p_fixnum((int64_t)argc);
        return false;
    }
    if (argc > 512u) return vm_fail(vm, P_ERR_STACK, "VM call has too many arguments");
    VMFrame *frames = vm_frames(vm, core);
    Value *frame_roots = vm_closure_roots(vm, core);
    bool replace = tail && core->frame_count != 0u &&
        core->dynamic_count == frames[core->frame_count - 1u].dynamic_base;
    size_t frame_index = replace ? core->frame_count - 1u : core->frame_count;
    size_t frame_limit = (vm->emergency_mode || core->unwind_active) ? core->frame_capacity :
        vm->config.frame_capacity;
    if (!replace && core->frame_count >= frame_limit)
        return vm_fail(vm, P_ERR_STACK, "VM frame stack exhausted");
    size_t local_base = replace ? frames[frame_index].locals_base : call_base + 1u;
    size_t return_sp = replace ? frames[frame_index].return_sp : call_base;
    size_t dynamic_base = replace ? frames[frame_index].dynamic_base : core->dynamic_count;
    size_t value_limit = (vm->emergency_mode || core->unwind_active) ?
        core->value_capacity :
        vm->config.value_capacity;
    if (local_base > value_limit || proto->locals > value_limit - local_base)
        return vm_fail(vm, P_ERR_STACK, "VM local slots exceed value stack capacity");
    if (proto->rest && argc > proto->required) {
        size_t rest_count = argc - proto->required;
        vm_sync_value_roots(vm, core);
        if (rest_count > SIZE_MAX / sizeof(PCons) ||
            !p_reserve(p, rest_count * sizeof(PCons))) return false;
    }
    Value arguments[512];
    Value *stack = vm_values(vm, core);
    for (size_t i = 0u; i < argc; ++i) arguments[i] = stack[call_base + 1u + i];
    frame_roots[frame_index] = function;
    for (size_t i = 0u; i < proto->locals; ++i) stack[local_base + i] = P_UNBOUND;
    for (size_t i = 0u; i < proto->required; ++i) stack[local_base + i] = arguments[i];
    if (proto->rest) {
        Value rest = p->nil;
        for (size_t i = argc; i > proto->required; --i) {
            Value cell = p_cons(p, arguments[i - 1u], rest);
            if (cell == 0u) return false;
            rest = cell;
        }
        stack[local_base + proto->required] = rest;
    }
    frames[frame_index].pc = 0u;
    frames[frame_index].locals_base = local_base;
    frames[frame_index].operand_base = local_base + proto->locals;
    frames[frame_index].return_sp = return_sp;
    frames[frame_index].dynamic_base = dynamic_base;
    if (!replace) ++core->frame_count;
    core->sp = frames[frame_index].operand_base;
    return true;
}

static bool vm_return(PVM *vm, VMCore *core, Value result)
{
    if (core->frame_count == 0u) return vm_fail(vm, P_ERR_IMAGE, "VM RETURN without a frame");
    VMFrame *frames = vm_frames(vm, core);
    size_t current = core->frame_count - 1u;
    size_t dynamic_base = frames[current].dynamic_base;
    if (core->dynamic_count > dynamic_base) {
        core->unwind_active = true;
        core->unwind_waiting = false;
        core->unwind_depth = dynamic_base;
        core->unwind_action = VM_UNWIND_RETURN;
        vm_save_unwind_values(vm);
        vm->unwind_values[P_MULTIPLE_VALUES_LIMIT] = result;
        return vm_continue_unwind(vm, core);
    }
    size_t index = --core->frame_count;
    size_t return_sp = frames[index].return_sp;
    if (!vm_restore_dynamics(vm, core, dynamic_base)) return false;
    vm_closure_roots(vm, core)[index] = 0u;
    if (vm->signaling_error && index == vm->error_hook_frame)
        vm->error_hook_completed = true;
    core->sp = return_sp;
    return vm_push(vm, core, result);
}

static bool vm_read_constant(PVM *vm, PProto *proto, uint32_t index, Value *value)
{
    PVector *constants = p_vector_ptr(proto->constants);
    if ((size_t)index >= constants->length) return vm_fail(vm, P_ERR_IMAGE, "VM constant index out of range");
    *value = constants->data[index];
    return true;
}

static bool vm_dispatch_call(PVM *vm, VMCore *core, size_t base,
                             size_t argc, bool tail, size_t baseline);

/* Turn the operands already on the value stack into an ordinary function
 * call window. Slow paths use the symbol named by the instruction so global
 * redefinition remains visible to compiled code. */
static bool vm_call_fallback(PVM *vm, VMCore *core, PProto *proto,
                             uint32_t symbol_index, size_t argc,
                             size_t baseline)
{
    Value *stack = vm_values(vm, core);
    Value function;
    if (!vm_read_constant(vm, proto, symbol_index, &function) ||
        p_type(function) != P_SYMBOL)
        return vm_fail(vm, P_ERR_IMAGE, "hot opcode fallback is not a symbol");
    if (core->sp < argc || core->sp >= core->value_capacity)
        return vm_fail(vm, P_ERR_STACK, "no room for hot opcode fallback call");
    size_t base = core->sp - argc;
    for (size_t i = argc; i > 0u; --i)
        stack[base + i] = stack[base + i - 1u];
    stack[base] = function;
    ++core->sp;
    vm_sync_value_roots(vm, core);
    return vm_dispatch_call(vm, core, base, argc, false, baseline);
}

static Value global_function(PVM *vm, Value designator)
{
    if (p_type(designator) != P_SYMBOL) return designator;
    Value function = p_symbol_ptr(designator)->function;
    if (function == P_UNBOUND) {
        (void)p_error(vm->state->p, P_ERR_UNDEFINED, "undefined VM function designator");
        vm->state->p->error.datum = designator;
        return 0u;
    }
    return function;
}

static bool vm_dispatch_call(PVM *vm, VMCore *core, size_t base,
                             size_t argc, bool tail, size_t baseline)
{
    PState *state = vm->state;
    Pocket *p = state->p;
    Value *stack = vm_values(vm, core);
    if (base > core->sp || argc + 1u > core->sp - base)
        return vm_fail(vm, P_ERR_IMAGE, "invalid VM call window");
    Value function = stack[base];
    for (;;) {
        if (p_type(function) == P_SYMBOL) {
            function = global_function(vm, function);
            if (function == 0u) return false;
        }
        stack[base] = function;
        if (p_type(function) == P_PRIMITIVE) {
            PPrimitive *primitive = (PPrimitive *)function;
            if ((primitive->flags & P_PRIMITIVE_EVAL_FUNCALL) != 0u) {
                if (argc == 0u) return vm_fail(vm, P_ERR_ARITY, "FUNCALL requires a function designator");
                function = stack[base + 1u];
                memmove(stack + base + 1u, stack + base + 2u,
                        (argc - 1u) * sizeof(Value));
                --argc;
                --core->sp;
                continue;
            }
            if ((primitive->flags & P_PRIMITIVE_EVAL_APPLY) != 0u) {
                if (argc < 2u) return vm_fail(vm, P_ERR_ARITY, "APPLY requires a function and a list");
                size_t prefix = argc - 2u;
                /* Keep the target designator in the rooted function slot
                 * while the final list is flattened into the argument area. */
                stack[base] = stack[base + 1u];
                Value list = stack[base + argc];
                if (prefix != 0u)
                    memmove(stack + base + 1u, stack + base + 2u,
                            prefix * sizeof(Value));
                size_t expanded = prefix;
                size_t value_limit = (vm->emergency_mode || core->unwind_active) ? core->value_capacity :
                    vm->config.value_capacity;
                while (p_type(list) == P_CONS) {
                    if (base >= value_limit || expanded >= value_limit - base - 1u)
                        return vm_fail(vm, P_ERR_STACK, "APPLY argument list is too long");
                    Value car = p_cons_ptr(list)->car;
                    list = p_cons_ptr(list)->cdr;
                    stack[base + 1u + expanded++] = car;
                }
                if (list != p->nil) return vm_fail(vm, P_ERR_TYPE, "APPLY requires a proper final list");
                function = stack[base];
                argc = expanded;
                core->sp = base + argc + 1u;
                continue;
            }
            if (argc < primitive->min_arity || argc > primitive->max_arity) {
                (void)p_error(p, P_ERR_ARITY, "VM primitive received the wrong number of arguments");
                p->error.datum = primitive->name;
                p->error.expected = p_fixnum((int64_t)argc);
                return false;
            }
            size_t reserve_bytes = 0u;
            if (primitive->reserve != NULL &&
                !primitive->reserve(state, argc, stack + base + 1u, &reserve_bytes))
                return false;
            vm_sync_value_roots(vm, core);
            if (reserve_bytes != 0u && !p_reserve(p, reserve_bytes)) return false;
            state->mv_count = 0u;
            Value result = primitive->entry(state, argc, stack + base + 1u);
            if (p->error.code != P_OK) return false;
            if (state->mv_count == 0u &&
                (primitive->flags & P_PRIMITIVE_EVAL_VALUES) == 0u) {
                state->mv_count = 1u;
                state->mv[0] = result;
            }
            if (result == 0u) return vm_fail(vm, P_ERR_UNSUPPORTED, "VM primitive failed without an error");
            Value primary = state->mv_count == 0u ? p->nil : state->mv[0];
            core->sp = base;
            if (tail && core->frame_count > baseline)
                return vm_return(vm, core, primary);
            return vm_push(vm, core, primary);
        }
        if (p_type(function) != P_CLOSURE)
            return vm_fail(vm, P_ERR_TYPE, "VM attempted to call a non-function");
        PClosure *closure = (PClosure *)function;
        if ((closure->flags & P_EVAL_COMPILED) != 0u) {
            if (!vm_enter_compiled(vm, core, function, base, argc, tail)) return false;
            return true;
        }
        if (state->interpreted_apply == NULL)
            return vm_fail(vm, P_ERR_UNSUPPORTED, "runtime VM cannot call an interpreted closure");
        /* CALL may have pushed its function and arguments since the previous
         * root synchronization. Stage-0 callbacks can allocate while binding
         * their arguments, so register the complete call window first. */
        vm_sync_value_roots(vm, core);
        Value result = state->interpreted_apply(state, function, argc, stack + base + 1u);
        if (p->error.code != P_OK) return false;
        (void)result;
        Value primary = state->mv_count == 0u ? p->nil : state->mv[0];
        core->sp = base;
        if (tail && core->frame_count > baseline)
            return vm_return(vm, core, primary);
        return vm_push(vm, core, primary);
    }
}

static bool vm_run(PVM *vm, size_t baseline)
{
    PState *state = vm->state;
    Pocket *p = state->p;
    VMCore *core = vm_core(vm);
    Value *stack = vm_values(vm, core);
    VMFrame *frames = vm_frames(vm, core);
    Value *closures = vm_closure_roots(vm, core);
    while (core->frame_count > baseline && p->error.code == P_OK &&
           !(vm->signaling_error && vm->error_hook_completed)) {
        if (core->unwind_active &&
            (!core->unwind_waiting || core->frame_count <= core->unwind_wait_frame)) {
            if (!vm_continue_unwind(vm, core)) return false;
            continue;
        }
        size_t frame_index = core->frame_count - 1u;
        PClosure *closure = (PClosure *)closures[frame_index];
        PProto *proto = (PProto *)closure->body;
        const uint8_t *code = vm_bytes(proto->code)->data;
        size_t code_length = vm_bytes(proto->code)->length;
        VMFrame *frame = &frames[frame_index];
        if (frame->pc >= code_length) return vm_fail(vm, P_ERR_IMAGE, "VM instruction pointer is outside code");
        size_t instruction = frame->pc++;
        uint8_t opcode = code[instruction];
        size_t width = 0u;
        (void)opcode_operand_width(opcode, &width);
        if (width > code_length - instruction - 1u)
            return vm_fail(vm, P_ERR_IMAGE, "truncated verified VM instruction");
        uint32_t operand = width == 4u ? read_u32(code + instruction + 1u) : 0u;
        frame->pc += width;
        Value value = 0u;
        PVector *constants = p_vector_ptr(proto->constants);
        switch ((POpcode)opcode) {
        case P_OP_CONST:
            if (!vm_read_constant(vm, proto, operand, &value) || !vm_push(vm, core, value)) return false;
            state->mv[0] = value; state->mv_count = 1u; break;
        case P_OP_GLOBAL: {
            if (!vm_read_constant(vm, proto, operand, &value)) return false;
            if (p_type(value) != P_SYMBOL) return vm_fail(vm, P_ERR_IMAGE, "GLOBAL operand is not a symbol");
            value = p_symbol_ptr(value)->value;
            if (value == P_UNBOUND) { (void)p_error(p, P_ERR_UNBOUND, "unbound global variable"); return false; }
            if (!vm_push(vm, core, value)) return false;
            state->mv[0] = value; state->mv_count = 1u; break;
        }
        case P_OP_SET_GLOBAL: {
            if (!vm_read_constant(vm, proto, operand, &value) || p_type(value) != P_SYMBOL)
                return vm_fail(vm, P_ERR_IMAGE, "SET_GLOBAL operand is not a symbol");
            if ((p_symbol_ptr(value)->flags & P_CONSTANT) != 0u)
                return vm_fail(vm, P_ERR_CONTROL, "cannot assign a constant global");
            if (core->sp == 0u) return vm_fail(vm, P_ERR_IMAGE, "SET_GLOBAL has no value");
            p_symbol_ptr(value)->value = stack[core->sp - 1u];
            state->mv[0] = stack[core->sp - 1u]; state->mv_count = 1u; break;
        }
        case P_OP_FUNCTION:
            if (!vm_read_constant(vm, proto, operand, &value) || p_type(value) != P_SYMBOL)
                return vm_fail(vm, P_ERR_IMAGE, "FUNCTION operand is not a symbol");
            value = p_symbol_ptr(value)->function;
            if (value == P_UNBOUND) return vm_fail(vm, P_ERR_UNDEFINED, "undefined global function");
            if (!vm_push(vm, core, value)) return false;
            state->mv[0] = value; state->mv_count = 1u; break;
        case P_OP_LOCAL:
            if ((size_t)operand >= proto->locals) return vm_fail(vm, P_ERR_IMAGE, "LOCAL index out of range");
            value = stack[frame->locals_base + operand];
            if (value == P_UNBOUND) return vm_fail(vm, P_ERR_UNBOUND, "unbound VM local");
            if (!vm_push(vm, core, value)) return false;
            state->mv[0] = value; state->mv_count = 1u; break;
        case P_OP_SET_LOCAL:
            if ((size_t)operand >= proto->locals || core->sp == 0u)
                return vm_fail(vm, P_ERR_IMAGE, "SET_LOCAL operand is invalid");
            stack[frame->locals_base + operand] = stack[core->sp - 1u];
            state->mv[0] = stack[core->sp - 1u]; state->mv_count = 1u; break;
        case P_OP_FREE: {
            PVector *captures = p_vector_ptr(closure->environment);
            if (p_type(closure->environment) != P_VECTOR || (size_t)operand >= captures->length)
                return vm_fail(vm, P_ERR_IMAGE, "FREE index out of range");
            value = captures->data[operand];
            if (!vm_push(vm, core, value)) return false;
            state->mv[0] = value; state->mv_count = 1u; break;
        }
        case P_OP_POP: (void)vm_pop(vm, core); if (p->error.code != P_OK) return false; break;
        case P_OP_DUP:
            if (core->sp == 0u || !vm_push(vm, core, stack[core->sp - 1u]))
                return vm_fail(vm, P_ERR_IMAGE, "DUP has no operand");
            break;
        case P_OP_JUMP: case P_OP_JUMP_NIL: {
            bool take = true;
            if (opcode == P_OP_JUMP_NIL) take = vm_pop(vm, core) == p->nil;
            if (p->error.code != P_OK) return false;
            if (take) {
                uint32_t raw = operand;
                int64_t rel = raw < UINT32_C(0x80000000) ? raw : (int64_t)raw - INT64_C(0x100000000);
                int64_t target = (int64_t)frame->pc + rel;
                if (target < 0 || (uint64_t)target >= code_length)
                    return vm_fail(vm, P_ERR_IMAGE, "branch target outside verified code");
                frame->pc = (size_t)target;
            }
            break;
        }
        case P_OP_CALL: case P_OP_TAIL_CALL: {
            size_t argc = operand;
            if (core->sp < argc + 1u) return vm_fail(vm, P_ERR_IMAGE, "CALL stack underflow");
            size_t base = core->sp - argc - 1u;
            size_t prior_frames = core->frame_count;
            if (!vm_dispatch_call(vm, core, base, argc,
                                  opcode == P_OP_TAIL_CALL, baseline)) return false;
            if (opcode == P_OP_TAIL_CALL && core->frame_count < prior_frames) break;
            break;
        }
        case P_OP_RETURN:
            value = vm_pop(vm, core);
            if (p->error.code != P_OK || !vm_return(vm, core, value)) return false;
            break;
        case P_OP_CLOSURE: {
            if (!vm_read_constant(vm, proto, operand, &value) || p_type(value) != P_PROTO)
                return vm_fail(vm, P_ERR_IMAGE, "CLOSURE constant is not a proto");
            PProto *child = (PProto *)value;
            size_t captures_count = child->free_count;
            if (core->sp < captures_count || p_type(child->constants) != P_VECTOR)
                return vm_fail(vm, P_ERR_IMAGE, "CLOSURE capture stack underflow");
            vm_sync_value_roots(vm, core);
            if (captures_count > (SIZE_MAX - sizeof(PVector)) / sizeof(Value) ||
                !p_reserve(p, offsetof(PVector, data) +
                           captures_count * sizeof(Value) + sizeof(PClosure)))
                return false;
            Value captures = p_vector(p, captures_count, p->nil);
            if (captures == 0u) return false;
            PVector *capture_vector = p_vector_ptr(captures);
            for (size_t i = 0u; i < captures_count; ++i)
                capture_vector->data[i] = stack[core->sp - captures_count + i];
            core->sp -= captures_count;
            value = p_vm_make_closure(p, value, captures);
            if (value == 0u || !vm_push(vm, core, value)) return false;
            state->mv[0] = value; state->mv_count = 1u; break;
        }
        case P_OP_BOX: {
            if (core->sp == 0u) return vm_fail(vm, P_ERR_IMAGE, "BOX has no value");
            vm_sync_value_roots(vm, core);
            if (!p_reserve(p, sizeof(PBox))) return false;
            PBox *box = (PBox *)p_alloc(p, P_BOX, sizeof(PBox));
            if (box == NULL) return false;
            box->value = stack[core->sp - 1u];
            stack[core->sp - 1u] = (Value)box;
            state->mv[0] = (Value)box; state->mv_count = 1u; break;
        }
        case P_OP_UNBOX:
            if (core->sp == 0u || p_type(stack[core->sp - 1u]) != P_BOX)
                return vm_fail(vm, P_ERR_TYPE, "UNBOX requires a box");
            stack[core->sp - 1u] = ((PBox *)stack[core->sp - 1u])->value;
            state->mv[0] = stack[core->sp - 1u]; state->mv_count = 1u; break;
        case P_OP_SET_BOX:
            if (core->sp < 2u || p_type(stack[core->sp - 2u]) != P_BOX)
                return vm_fail(vm, P_ERR_TYPE, "SET_BOX requires a box and value");
            ((PBox *)stack[core->sp - 2u])->value = stack[core->sp - 1u];
            stack[core->sp - 2u] = stack[core->sp - 1u]; --core->sp;
            state->mv[0] = stack[core->sp - 1u]; state->mv_count = 1u; break;
        case P_OP_VALUES: {
            size_t count = operand;
            if (count > P_MULTIPLE_VALUES_LIMIT || core->sp < count)
                return vm_fail(vm, P_ERR_STACK, "VALUES count exceeds the multiple-value limit");
            if (count == 0u) {
                state->mv[0] = p->nil; state->mv_count = 0u;
                if (!vm_push(vm, core, p->nil)) return false;
            } else {
                memcpy(state->mv, stack + core->sp - count, count * sizeof(Value));
                state->mv_count = count;
                core->sp -= count;
                if (!vm_push(vm, core, state->mv[0])) return false;
            }
            break;
        }
        case P_OP_MV_LIST: {
            if (core->sp == 0u) return vm_fail(vm, P_ERR_IMAGE, "MV_LIST has no primary value");
            size_t count = state->mv_count;
            vm_sync_value_roots(vm, core);
            if (count > P_MULTIPLE_VALUES_LIMIT ||
                count > SIZE_MAX / sizeof(PCons) ||
                (count != 0u && !p_reserve(p, count * sizeof(PCons)))) return false;
            Value list = p->nil;
            for (size_t i = count; i > 0u; --i) {
                list = p_cons(p, state->mv[i - 1u], list);
                if (list == 0u) return false;
            }
            --core->sp;
            stack[core->sp++] = list;
            state->mv[0] = list; state->mv_count = 1u; break;
        }
        case P_OP_MV_RESTORE: {
            if (core->sp == 0u) return vm_fail(vm, P_ERR_IMAGE, "MV_RESTORE has no list");
            Value list = vm_pop(vm, core);
            size_t count = 0u;
            while (p_type(list) == P_CONS) {
                if (count == P_MULTIPLE_VALUES_LIMIT)
                    return vm_fail(vm, P_ERR_STACK, "too many values in MV_RESTORE");
                state->mv[count++] = p_cons_ptr(list)->car;
                list = p_cons_ptr(list)->cdr;
            }
            if (list != p->nil) return vm_fail(vm, P_ERR_TYPE, "MV_RESTORE requires a proper list");
            state->mv_count = count;
            value = count == 0u ? p->nil : state->mv[0];
            if (!vm_push(vm, core, value)) return false;
            break;
        }
        case P_OP_BIND_SPECIAL: {
            if (!vm_read_constant(vm, proto, operand, &value) || p_type(value) != P_SYMBOL || core->sp == 0u)
                return vm_fail(vm, P_ERR_IMAGE, "BIND_SPECIAL operand is invalid");
            Value new_value = vm_pop(vm, core);
            Value old_value = p_symbol_ptr(value)->value;
            if (!vm_dynamic_push(vm, core, VM_DYN_SPECIAL, value, old_value,
                                 p->nil, 0u, core->sp)) return false;
            p_symbol_ptr(value)->value = new_value;
            break;
        }
        case P_OP_PUSH_CATCH: {
            if (core->sp == 0u) return vm_fail(vm, P_ERR_IMAGE, "PUSH_CATCH has no tag");
            Value tag = vm_pop(vm, core);
            int64_t rel = operand < UINT32_C(0x80000000) ? operand : (int64_t)operand - INT64_C(0x100000000);
            int64_t target = (int64_t)frame->pc + rel;
            if (target < 0 || (uint64_t)target >= code_length ||
                !vm_dynamic_push(vm, core, VM_DYN_CATCH, tag, p->nil, p->nil,
                                 (size_t)target, core->sp)) return false;
            break;
        }
        case P_OP_PUSH_BLOCK: {
            if (state->next_control_token >= (uint64_t)p_fixnum_max())
                return vm_fail(vm, P_ERR_CONTROL, "VM control token space exhausted");
            Value token = p_fixnum((int64_t)++state->next_control_token);
            int64_t rel = operand < UINT32_C(0x80000000) ? operand : (int64_t)operand - INT64_C(0x100000000);
            int64_t target = (int64_t)frame->pc + rel;
            if (target < 0 || (uint64_t)target >= code_length ||
                !vm_dynamic_push(vm, core, VM_DYN_BLOCK, token, p->nil, p->nil,
                                 (size_t)target, core->sp) ||
                !vm_push(vm, core, token)) return false;
            break;
        }
        case P_OP_PUSH_TAGBODY: {
            if (!vm_read_constant(vm, proto, operand, &value) || p_type(value) != P_VECTOR)
                return vm_fail(vm, P_ERR_IMAGE, "PUSH_TAGBODY table is invalid");
            if (state->next_control_token >= (uint64_t)p_fixnum_max())
                return vm_fail(vm, P_ERR_CONTROL, "VM control token space exhausted");
            Value token = p_fixnum((int64_t)++state->next_control_token);
            if (!vm_dynamic_push(vm, core, VM_DYN_TAGBODY, token, value, p->nil,
                                 0u, core->sp) || !vm_push(vm, core, token)) return false;
            vm_dynamics(vm, core)[core->dynamic_count - 1u].label_count =
                p_vector_ptr(value)->length;
            break;
        }
        case P_OP_PUSH_UNWIND:
            if (core->sp == 0u) return vm_fail(vm, P_ERR_IMAGE, "PUSH_UNWIND has no cleanup function");
            value = vm_pop(vm, core);
            if (!vm_dynamic_push(vm, core, VM_DYN_UNWIND, value, p->nil,
                                 p->nil, 0u, core->sp)) return false;
            break;
        case P_OP_POP_DYNAMIC:
            if ((size_t)operand > core->dynamic_count ||
                !vm_restore_dynamics(vm, core, core->dynamic_count - operand)) return false;
            break;
        case P_OP_THROW: case P_OP_RETURN_FROM: {
            if (core->sp < 2u)
                return vm_fail(vm, P_ERR_IMAGE, "nonlocal transfer lacks tag/token and value");
            Value key = stack[core->sp - 2u];
            value = stack[core->sp - 1u];
            core->sp -= 2u;
            /* The transfer operand supplies the primary value on the VM
             * stack.  Its producer's full multiple-value packet remains in
             * PState::mv and must cross cleanup unchanged (including zero
             * values).  Replacing it with the primary here silently broke
             * THROW and RETURN-FROM of multiple-value forms. */
            if (state->mv_count == 0u) state->mv[0] = p->nil;
            else if (state->mv[0] != value) {
                /* Stack plumbing may have changed the primary without
                 * changing the packet; the transfer operand is authoritative
                 * for value zero, while secondary values remain intact. */
                state->mv[0] = value;
            }
            if (!vm_transfer(vm, core,
                             opcode == P_OP_THROW ? VM_DYN_CATCH : VM_DYN_BLOCK,
                             key, 0u)) return false;
            break;
        }
        case P_OP_GO:
            if (core->sp == 0u) return vm_fail(vm, P_ERR_IMAGE, "GO has no tagbody token");
            value = vm_pop(vm, core);
            if (p->error.code != P_OK) return false;
            if (!vm_transfer(vm, core, VM_DYN_TAGBODY, value, (size_t)operand))
                return false;
            break;
        case P_OP_ADD: case P_OP_SUB: case P_OP_LT: case P_OP_NUM_EQ: {
            const char *name = opcode == P_OP_ADD ? "+" :
                opcode == P_OP_SUB ? "-" : opcode == P_OP_LT ? "<" : "=";
            if (!vm_read_constant(vm, proto, operand, &value) ||
                !p_is_builtin_primitive(p, value, name)) {
                if (!vm_call_fallback(vm, core, proto, operand, 2u, baseline)) return false;
                break;
            }
            if (core->sp < 2u) return vm_fail(vm, P_ERR_IMAGE, "binary VM instruction underflow");
            Value left = stack[core->sp - 2u], right = stack[core->sp - 1u];
            if (!p_is_fixnum(left) || !p_is_fixnum(right)) {
                if (!vm_call_fallback(vm, core, proto, operand, 2u, baseline)) return false;
                break;
            }
            int64_t a = p_fixnum_value(left), b = p_fixnum_value(right), integer = 0;
            if (opcode == P_OP_LT || opcode == P_OP_NUM_EQ) {
                value = (opcode == P_OP_LT ? a < b : a == b) ? p->truth : p->nil;
            } else {
                if ((opcode == P_OP_ADD && ((b > 0 && a > p_fixnum_max() - b) ||
                                             (b < 0 && a < p_fixnum_min() - b))) ||
                    (opcode == P_OP_SUB && ((b < 0 && a > p_fixnum_max() + b) ||
                                             (b > 0 && a < p_fixnum_min() + b))))
                {
                    if (!vm_call_fallback(vm, core, proto, operand, 2u, baseline)) return false;
                    break;
                }
                integer = opcode == P_OP_ADD ? a + b : a - b;
                value = p_fixnum(integer);
            }
            core->sp -= 2u;
            if (!vm_push(vm, core, value)) return false;
            state->mv[0] = value; state->mv_count = 1u; break;
        }
        case P_OP_CONS:
            if (!vm_read_constant(vm, proto, operand, &value) ||
                !p_is_builtin_primitive(p, value, "CONS")) {
                if (!vm_call_fallback(vm, core, proto, operand, 2u, baseline)) return false;
                break;
            }
            if (core->sp < 2u) return vm_fail(vm, P_ERR_IMAGE, "CONS has too few operands");
            vm_sync_value_roots(vm, core);
            if (!p_reserve(p, sizeof(PCons))) return false;
            value = p_cons(p, stack[core->sp - 2u], stack[core->sp - 1u]);
            if (value == 0u) return false;
            core->sp -= 2u;
            if (!vm_push(vm, core, value)) return false;
            state->mv[0] = value; state->mv_count = 1u; break;
        case P_OP_CAR: case P_OP_CDR:
            if (!vm_read_constant(vm, proto, operand, &value) ||
                !p_is_builtin_primitive(p, value,
                                        opcode == P_OP_CAR ? "CAR" : "CDR")) {
                if (!vm_call_fallback(vm, core, proto, operand, 1u, baseline)) return false;
                break;
            }
            if (core->sp == 0u) return vm_fail(vm, P_ERR_IMAGE, "unary VM instruction underflow");
            value = stack[core->sp - 1u];
            if (value == p->nil) value = p->nil;
            else if (p_type(value) != P_CONS) {
                if (!vm_call_fallback(vm, core, proto, operand, 1u, baseline)) return false;
                break;
            }
            else value = opcode == P_OP_CAR ? p_cons_ptr(value)->car : p_cons_ptr(value)->cdr;
            stack[core->sp - 1u] = value;
            state->mv[0] = value; state->mv_count = 1u; break;
        default:
            (void)constants;
            return vm_fail(vm, P_ERR_IMAGE, "unknown verified VM opcode");
        }
        (void)constants;
    }
    return p->error.code == P_OK;
}

static void vm_clear_error_hook(PVM *vm, bool restore_original)
{
    if (vm == NULL || !vm->signaling_error) return;
    Pocket *p = vm->state->p;
    if (restore_original) p->error = vm->pending_error;
    p_root_restore(p, vm->error_root_base);
    vm->signaling_error = false;
    vm->error_hook_completed = false;
    memset(&vm->pending_error, 0, sizeof(vm->pending_error));
    vm->error_values[0] = vm->error_values[1] = 0u;
}

static void vm_release_emergency_heap(PVM *vm)
{
    if (vm->emergency_heap_reserve == 0u) return;
    vm->emergency_heap_reserve = 0u;
    p_gc(vm->state->p);
}

static void vm_replenish_emergency_heap(PVM *vm)
{
    if (vm->emergency_heap_reserve != 0u || vm->config.emergency_bytes == 0u)
        return;
    Pocket *p = vm->state->p;
    PError saved = p->error;
    size_t bytes = offsetof(PBytes, data) + vm->config.emergency_bytes;
    if (bytes >= offsetof(PBytes, data) && p_reserve(p, bytes))
        vm->emergency_heap_reserve = p_bytes(p, vm->config.emergency_bytes);
    p->error = saved;
}

static bool vm_begin_error_unwind(PVM *vm, VMCore *core, size_t depth)
{
    Pocket *p = vm->state->p;
    if (p->error.code == P_OK || core->dynamic_count <= depth) return false;
    vm->pending_error = p->error;
    vm->error_values[0] = vm->pending_error.datum;
    vm->error_values[1] = vm->pending_error.expected;
    vm->error_root_base = p_root_save(p);
    if (!p_roots(p, vm->error_values, 2u)) return false;
    memset(&p->error, 0, sizeof(p->error));
    core->unwind_action = VM_UNWIND_ERROR;
    return vm_restore_dynamics(vm, core, depth);
}

/* Start the ordinary Lisp error hook on the active VM stack.  The return
 * continuation is represented by the VM state rather than a nested C call,
 * so a handler can THROW into a catch that was already active at the fault. */
static bool vm_start_error_hook(PVM *vm, size_t baseline)
{
    Pocket *p = vm->state->p;
    VMCore *core = vm_core(vm);
    Value hook = p->error_hook;
    if (hook == 0u || hook == p->nil || p->error.code == P_OK ||
        vm->signaling_error || p->error.signaled) return false;
    if (p_type(hook) != P_CLOSURE && p_type(hook) != P_PRIMITIVE)
        return false;

    vm->pending_error = p->error;
    vm->error_values[0] = vm->pending_error.datum;
    vm->error_values[1] = vm->pending_error.expected;
    vm->error_root_base = p_root_save(p);
    if (!p_roots(p, vm->error_values, 2u)) {
        memset(&vm->pending_error, 0, sizeof(vm->pending_error));
        return false;
    }
    vm_release_emergency_heap(vm);
    memset(&p->error, 0, sizeof(p->error));
    vm->emergency_mode = true;
    vm->signaling_error = true;
    vm->error_hook_completed = false;
    size_t hook_baseline = core->frame_count;
    vm->error_hook_frame = hook_baseline;

    size_t base = core->sp;
    if (base > core->value_capacity || core->value_capacity - base < 4u) {
        vm_clear_error_hook(vm, true);
        return false;
    }
    Value *stack = vm_values(vm, core);
    stack[core->sp++] = hook;
    stack[core->sp++] = p_fixnum((int64_t)vm->pending_error.code);
    stack[core->sp++] = vm->error_values[0] == 0u ? p->nil : vm->error_values[0];
    stack[core->sp++] = vm->error_values[1] == 0u ? p->nil : vm->error_values[1];
    vm_sync_value_roots(vm, core);
    if (!vm_dispatch_call(vm, core, base, 3u, false, hook_baseline)) {
        vm_clear_error_hook(vm, true);
        return false;
    }
    if (core->frame_count == hook_baseline)
        vm->error_hook_completed = true; /* Primitive hook returned directly. */
    (void)baseline;
    return true;
}

Value p_vm_call(PVM *vm, Value function, size_t argc, const Value *argv)
{
    if (vm == NULL || !vm->initialized || vm->state == NULL) return 0u;
    VMCore *core = vm_core(vm);
    PState *state = vm->state;
    Pocket *p = state->p;
    bool previous_emergency_mode = vm->emergency_mode;
    if (p_type(function) == P_CLOSURE &&
        (((PClosure *)function)->flags & P_EVAL_COMPILED) == 0u) {
        if (state->interpreted_apply == NULL) {
            (void)p_error(p, P_ERR_UNSUPPORTED, "runtime VM cannot call an interpreted closure");
            return 0u;
        }
        return state->interpreted_apply(state, function, argc, argv);
    }
    size_t entry_limit = vm->emergency_mode ? core->value_capacity :
        vm->config.value_capacity;
    if (argc > 512u || core->sp > entry_limit ||
        argc + 1u > entry_limit - core->sp) {
        (void)p_error(p, P_ERR_STACK, "VM entry argument window exceeds its stack");
        return 0u;
    }
    size_t baseline = core->frame_count;
    size_t baseline_dynamic = core->dynamic_count;
    size_t call_base = core->sp;
    Value *stack = vm_values(vm, core);
    stack[core->sp++] = function;
    for (size_t i = 0u; i < argc; ++i) stack[core->sp++] = argv[i];
    vm_sync_value_roots(vm, core);
    bool started = vm_dispatch_call(vm, core, call_base, argc, false, baseline);
    bool succeeded = false;
    for (;;) {
        if (!started || p->error.code != P_OK) {
            if (!vm_start_error_hook(vm, baseline)) break;
            started = true;
            continue;
        }
        if (!vm_run(vm, baseline) || p->error.code != P_OK) {
            if (vm->signaling_error) {
                vm_clear_error_hook(vm, true);
                break;
            }
            if (!vm_start_error_hook(vm, baseline)) break;
            started = true;
            continue;
        }
        if (vm->signaling_error && vm->error_hook_completed) {
            vm_clear_error_hook(vm, true);
            break;
        }
        succeeded = true;
        break;
    }
    if (!succeeded || p->error.code != P_OK) {
        if (p->error.code != P_OK && core->dynamic_count > baseline_dynamic &&
            !vm->signaling_error) {
            bool queued = vm_begin_error_unwind(vm, core, baseline_dynamic);
            if (queued && core->unwind_active) (void)vm_run(vm, baseline);
        }
        core->sp = call_base;
        while (core->frame_count > baseline) {
            size_t index = --core->frame_count;
            vm_closure_roots(vm, core)[index] = 0u;
        }
        vm_sync_value_roots(vm, core);
        vm->emergency_mode = previous_emergency_mode;
        if (!previous_emergency_mode) vm_replenish_emergency_heap(vm);
        return 0u;
    }
    Value result = core->sp > call_base ? stack[call_base] : p->nil;
    core->sp = call_base;
    vm_sync_value_roots(vm, core);
    vm->emergency_mode = previous_emergency_mode;
    if (!previous_emergency_mode) vm_replenish_emergency_heap(vm);
    return result;
}

static bool proto_target(Pocket *p, PProto *proto, size_t instruction_end,
                         uint32_t encoded, size_t *target)
{
    int64_t displacement = encoded < UINT32_C(0x80000000) ?
        (int64_t)encoded : (int64_t)encoded - INT64_C(0x100000000);
    if (instruction_end > SIZE_MAX - (size_t)(displacement > 0 ? displacement : 0))
        return false;
    int64_t result = (int64_t)instruction_end + displacement;
    size_t code_length = vm_bytes(proto->code)->length;
    if (result < 0 || (uint64_t)result > code_length) {
        (void)p_error(p, P_ERR_IMAGE, "VM branch target is outside byte code");
        return false;
    }
    *target = (size_t)result;
    (void)p;
    return true;
}

bool p_vm_verify(Pocket *p, Value proto_value, void *workspace,
                 size_t workspace_size)
{
    if (p == NULL || p_type(proto_value) != P_PROTO) {
        if (p != NULL) (void)p_error(p, P_ERR_TYPE, "VM verifier requires a proto");
        return false;
    }
    PProto *proto = (PProto *)proto_value;
    if (p_type(proto->code) != P_BYTES || p_type(proto->constants) != P_VECTOR ||
        proto->target_bits != p_fixnum_bits() || proto->required > proto->locals ||
        (proto->rest && proto->required >= proto->locals)) {
        (void)p_error(p, P_ERR_IMAGE, "proto metadata is inconsistent with this runtime");
        return false;
    }
    size_t length = vm_bytes(proto->code)->length;
    size_t expected;
    if (!p_vm_verify_workspace(length, &expected) || workspace_size < expected) {
        (void)p_error(p, P_ERR_RANGE, "VM verifier workspace is too small");
        return false;
    }
    size_t entries = length + 1u;
    uint8_t *boundary;
    size_t *arrays = workspace_arrays(workspace, workspace_size, entries, &boundary);
    if (arrays == NULL) {
        (void)p_error(p, P_ERR_RANGE, "VM verifier workspace is misaligned or too small");
        return false;
    }
    size_t *depths = arrays;
    size_t *queue = arrays + entries;
    memset(boundary, 0, entries);
    for (size_t i = 0u; i < entries; ++i) depths[i] = SIZE_MAX;
    const uint8_t *code = vm_bytes(proto->code)->data;
    size_t pc = 0u;
    while (pc < length) {
        boundary[pc] = 1u;
        size_t operand_width;
        if (!opcode_operand_width(code[pc], &operand_width) ||
            operand_width > length - pc - 1u) {
            (void)p_error(p, P_ERR_IMAGE, "unknown or truncated VM instruction");
            return false;
        }
        pc += 1u + operand_width;
    }
    boundary[length] = 1u;
    if (length == 0u) {
        (void)p_error(p, P_ERR_IMAGE, "empty VM proto");
        return false;
    }

    /* Validate references in dead code too. Reachability is a stack analysis
     * concern; malformed indices or jumps must never be hidden behind an
     * unconditional branch. */
    for (pc = 0u; pc < length;) {
        uint8_t opcode = code[pc];
        size_t width = 0u;
        (void)opcode_operand_width(opcode, &width);
        size_t end = pc + 1u + width;
        uint32_t operand = width == 4u ? read_u32(code + pc + 1u) : 0u;
        bool symbol_operand = opcode == P_OP_GLOBAL || opcode == P_OP_SET_GLOBAL ||
            opcode == P_OP_FUNCTION || opcode == P_OP_BIND_SPECIAL ||
            opcode == P_OP_ADD || opcode == P_OP_SUB || opcode == P_OP_LT ||
            opcode == P_OP_NUM_EQ || opcode == P_OP_CONS || opcode == P_OP_CAR ||
            opcode == P_OP_CDR;
        if (symbol_operand && !verify_constant(p, proto, operand, true)) goto bad_reference;
        if (opcode == P_OP_CONST && !verify_constant(p, proto, operand, false))
            goto bad_reference;
        if (opcode == P_OP_CLOSURE) {
            if (!verify_constant(p, proto, operand, false) ||
                p_type(p_vector_ptr(proto->constants)->data[operand]) != P_PROTO)
                goto bad_reference;
        }
        if (opcode == P_OP_JUMP || opcode == P_OP_JUMP_NIL ||
            opcode == P_OP_PUSH_CATCH || opcode == P_OP_PUSH_BLOCK) {
            size_t target;
            if (!proto_target(p, proto, end, operand, &target) ||
                target >= length || !boundary[target]) goto bad_reference;
        }
        if (opcode == P_OP_LOCAL && (size_t)operand >= proto->locals)
            goto bad_reference;
        if (opcode == P_OP_FREE && (size_t)operand >= proto->free_count)
            goto bad_reference;
        if ((opcode == P_OP_CALL || opcode == P_OP_TAIL_CALL) && operand > 512u)
            goto bad_reference;
        if (opcode == P_OP_VALUES && operand > P_MULTIPLE_VALUES_LIMIT)
            goto bad_reference;
        if (opcode == P_OP_PUSH_TAGBODY) {
            if (!verify_constant(p, proto, operand, false)) goto bad_reference;
            Value table = p_vector_ptr(proto->constants)->data[operand];
            if (p_type(table) != P_VECTOR) goto bad_reference;
            for (size_t i = 0u; i < p_vector_ptr(table)->length; ++i) {
                Value label = p_vector_ptr(table)->data[i];
                if (!p_is_fixnum(label) || p_fixnum_value(label) < 0 ||
                    (uint64_t)p_fixnum_value(label) >= length ||
                    !boundary[(size_t)p_fixnum_value(label)]) goto bad_reference;
            }
        }
        /* GO's label index is checked against the active lexical TAGBODY at
         * execution time. The matching token and table are control-flow
         * dependent, so rejecting it here would require a second dynamic
         * scope analysis; vm_transfer bounds-checks the index before lookup. */
        pc = end;
    }

    size_t head = 0u, tail = 0u;
    depths[0] = 0u;
    queue[tail++] = 0u;
    while (head < tail) {
        pc = queue[head++];
        if (pc == length) {
            (void)p_error(p, P_ERR_IMAGE, "VM control can fall off byte code");
            return false;
        }
        uint8_t opcode = code[pc];
        size_t width = 0u, end;
        (void)opcode_operand_width(opcode, &width);
        end = pc + 1u + width;
        uint32_t operand = width == 4u ? read_u32(code + pc + 1u) : 0u;
        size_t depth = depths[pc], need = 0u;
        int64_t effect = 0;
        bool terminal = false, branch = false, conditional = false;
        size_t branch_target = 0u;
        bool symbol_operand = false;
        switch ((POpcode)opcode) {
        case P_OP_CONST: case P_OP_GLOBAL: case P_OP_FUNCTION:
            effect = 1; symbol_operand = opcode != P_OP_CONST; break;
        case P_OP_SET_GLOBAL: case P_OP_SET_LOCAL: need = 1; symbol_operand = opcode == P_OP_SET_GLOBAL; break;
        case P_OP_LOCAL:
            if ((size_t)operand >= proto->locals) goto bad_index;
            effect = 1; break;
        case P_OP_FREE:
            if ((size_t)operand >= proto->free_count) goto bad_index;
            effect = 1; break;
        case P_OP_POP: need = 1; effect = -1; break;
        case P_OP_DUP: need = 1; effect = 1; break;
        case P_OP_JUMP: case P_OP_JUMP_NIL:
            if (!proto_target(p, proto, end, operand, &branch_target)) return false;
            branch = true; conditional = opcode == P_OP_JUMP_NIL;
            if (conditional) { need = 1; effect = -1; }
            break;
        case P_OP_CALL: need = (size_t)operand + 1u; effect = -(int64_t)operand; break;
        case P_OP_TAIL_CALL: need = (size_t)operand + 1u; terminal = true; break;
        case P_OP_RETURN: need = 1; terminal = true; break;
        case P_OP_CLOSURE: {
            need = 0;
            if (!verify_constant(p, proto, operand, false)) goto bad_index;
            Value child = p_vector_ptr(proto->constants)->data[operand];
            if (p_type(child) != P_PROTO) goto bad_index;
            size_t captures = ((PProto *)child)->free_count;
            if (captures > INT64_MAX) goto bad_index;
            need = captures; effect = 1 - (int64_t)captures;
            break;
        }
        case P_OP_BOX: case P_OP_UNBOX: need = 1; break;
        case P_OP_SET_BOX: need = 2; effect = -1; break;
        case P_OP_VALUES:
            need = operand; effect = 1 - (int64_t)operand; break;
        case P_OP_MV_LIST: case P_OP_MV_RESTORE: need = 1; break;
        case P_OP_BIND_SPECIAL: need = 1; effect = -1; symbol_operand = true; break;
        case P_OP_PUSH_CATCH:
            need = 1; effect = -1;
            if (!proto_target(p, proto, end, operand, &branch_target)) return false;
            branch = true; conditional = true;
            break;
        case P_OP_PUSH_BLOCK:
            effect = 1;
            if (!proto_target(p, proto, end, operand, &branch_target)) return false;
            branch = true; conditional = true;
            break;
        case P_OP_PUSH_TAGBODY: {
            if (!verify_constant(p, proto, operand, false)) goto bad_index;
            Value table = p_vector_ptr(proto->constants)->data[operand];
            if (p_type(table) != P_VECTOR) goto bad_index;
            effect = 1;
            for (size_t j = 0u; j < p_vector_ptr(table)->length; ++j) {
                Value label = p_vector_ptr(table)->data[j];
                if (!p_is_fixnum(label) || p_fixnum_value(label) < 0 ||
                    (uint64_t)p_fixnum_value(label) >= length ||
                    !boundary[(size_t)p_fixnum_value(label)]) goto bad_index;
                /* GO restores the stack height recorded before the token
                 * was pushed. Validate each possible label at that height,
                 * including labels without an ordinary fallthrough edge. */
                size_t target = (size_t)p_fixnum_value(label);
                if (depths[target] == SIZE_MAX) {
                    depths[target] = depth;
                    queue[tail++] = target;
                } else if (depths[target] != depth) goto bad_depth;
            }
            break;
        }
        case P_OP_PUSH_UNWIND: need = 1; effect = -1; break;
        case P_OP_POP_DYNAMIC: break;
        case P_OP_THROW: case P_OP_RETURN_FROM: need = 2; effect = -2; terminal = true; break;
        case P_OP_GO: need = 1; effect = -1; terminal = true; break;
        case P_OP_ADD: case P_OP_SUB: case P_OP_LT: case P_OP_NUM_EQ:
        case P_OP_CONS: need = 2; effect = -1; symbol_operand = true; break;
        case P_OP_CAR: case P_OP_CDR: need = 1; symbol_operand = true; break;
        default: goto bad_index;
        }
        if (symbol_operand && !verify_constant(p, proto, operand, true)) goto bad_index;
        if (depth < need || effect < -(int64_t)depth) {
            (void)p_error(p, P_ERR_IMAGE, "VM operand stack underflow");
            return false;
        }
        size_t after = (size_t)((int64_t)depth + effect);
        if (after > proto->max_stack) {
            (void)p_error(p, P_ERR_IMAGE, "proto max-stack declaration is too small");
            return false;
        }
        if (branch) {
            size_t branch_depth = after;
            if (opcode == P_OP_PUSH_CATCH) branch_depth = depth;
            if (opcode == P_OP_PUSH_BLOCK) branch_depth = depth + 1u;
            size_t target = branch_target;
            if (target >= length || !boundary[target]) goto bad_index;
            if (depths[target] == SIZE_MAX) { depths[target] = branch_depth; queue[tail++] = target; }
            else if (depths[target] != branch_depth) goto bad_depth;
        }
        if (!terminal && opcode != P_OP_JUMP) {
            if (end > length) goto bad_index;
            if (depths[end] == SIZE_MAX) { depths[end] = after; queue[tail++] = end; }
            else if (depths[end] != after) goto bad_depth;
        }
        continue;
bad_index:
        (void)p_error(p, P_ERR_IMAGE, "VM instruction operand is invalid");
        return false;
bad_depth:
        (void)p_error(p, P_ERR_IMAGE, "VM control-flow join has inconsistent stack depth");
        return false;
    }
    proto->flags |= P_PROTO_VERIFIED;
    return true;
bad_reference:
    (void)p_error(p, P_ERR_IMAGE, "VM instruction operand is invalid");
    return false;
}

/* VM-facing primitives keep the compiler and the image loader on the same
 * object ABI. Their reserve hooks perform every possible collection before
 * the callback starts; callbacks themselves only allocate from the already
 * reserved arena and never call back into Lisp. */
static bool vm_reserve_proto(PState *state, size_t argc, const Value *argv,
                             size_t *bytes)
{
    (void)state;
    if (argc != P_PROTO_FIELD_COUNT || p_type(argv[P_PROTO_CODE]) != P_BYTES) {
        *bytes = 0u;
        return true;
    }
    size_t workspace;
    if (!p_vm_verify_workspace(vm_bytes(argv[P_PROTO_CODE])->length, &workspace) ||
        workspace > SIZE_MAX - sizeof(PProto) - offsetof(PBytes, data)) {
        *bytes = 0u;
        return false;
    }
    *bytes = workspace + sizeof(PProto) + offsetof(PBytes, data);
    return true;
}

static Value vm_primitive_make_proto(PState *state, size_t argc,
                                     const Value *argv)
{
    if (argc != P_PROTO_FIELD_COUNT) return 0u;
    Value proto = p_vm_make_proto(state->p, argv);
    if (proto == 0u) return 0u;
    size_t workspace_size;
    if (!p_vm_verify_workspace(vm_bytes(((PProto *)proto)->code)->length,
                               &workspace_size)) {
        (void)p_error(state->p, P_ERR_RANGE, "VM verifier workspace size overflow");
        return 0u;
    }
    Value workspace = p_bytes(state->p, workspace_size);
    if (workspace == 0u ||
        !p_vm_verify(state->p, proto, vm_bytes(workspace)->data,
                     workspace_size)) return 0u;
    return proto;
}

static bool vm_reserve_closure(PState *state, size_t argc, const Value *argv,
                               size_t *bytes)
{
    (void)state; (void)argv;
    *bytes = argc == 2u ? sizeof(PClosure) : 0u;
    return true;
}

static Value vm_primitive_make_closure(PState *state, size_t argc,
                                       const Value *argv)
{
    if (argc != 2u) return 0u;
    return p_vm_make_closure(state->p, argv[0], argv[1]);
}

static Value vm_primitive_proto_ref(PState *state, size_t argc,
                                    const Value *argv)
{
    if (argc != 2u || p_type(argv[0]) != P_PROTO || !p_is_fixnum(argv[1])) {
        (void)p_error(state->p, P_ERR_TYPE, "%PROTO-REF expects a proto and field index");
        return 0u;
    }
    int64_t field = p_fixnum_value(argv[1]);
    if (field < 0 || field >= P_PROTO_FIELD_COUNT) {
        (void)p_error(state->p, P_ERR_RANGE, "%PROTO-REF field index is out of range");
        return 0u;
    }
    PProto *proto = (PProto *)argv[0];
    switch ((PProtoField)field) {
    case P_PROTO_NAME: return proto->name;
    case P_PROTO_REQUIRED: return p_fixnum((int64_t)proto->required);
    case P_PROTO_REST: return proto->rest ? state->p->truth : state->p->nil;
    case P_PROTO_LOCALS: return p_fixnum((int64_t)proto->locals);
    case P_PROTO_FREE: return p_fixnum((int64_t)proto->free_count);
    case P_PROTO_STACK: return p_fixnum((int64_t)proto->max_stack);
    case P_PROTO_TARGET_BITS: return p_fixnum((int64_t)proto->target_bits);
    case P_PROTO_CODE: return proto->code;
    case P_PROTO_CONSTANTS: return proto->constants;
    default: break;
    }
    return 0u;
}

static Value vm_primitive_special_p(PState *state, size_t argc,
                                    const Value *argv)
{
    if (argc != 1u || p_type(argv[0]) != P_SYMBOL) {
        (void)p_error(state->p, P_ERR_TYPE, "%SPECIAL-P expects a symbol");
        return 0u;
    }
    return (p_symbol_ptr(argv[0])->flags & P_SPECIAL) != 0u ?
        state->p->truth : state->p->nil;
}

bool p_install_vm_primitives(PState *state)
{
    if (state == NULL || !state->initialized) return false;
    return p_add_primitive(state, "%MAKE-PROTO", false,
                           vm_primitive_make_proto, vm_reserve_proto,
                           P_PROTO_FIELD_COUNT, P_PROTO_FIELD_COUNT, 0u) &&
           p_add_primitive(state, "%PROTO-REF", false,
                           vm_primitive_proto_ref, NULL, 2u, 2u, 0u) &&
           p_add_primitive(state, "%MAKE-CLOSURE", false,
                           vm_primitive_make_closure, vm_reserve_closure,
                           2u, 2u, 0u) &&
           p_add_primitive(state, "%SPECIAL-P", false,
                           vm_primitive_special_p, NULL, 1u, 1u, 0u);
}
