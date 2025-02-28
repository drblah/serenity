/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/IteratorOperations.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/SetConstructor.h>

namespace JS {

SetConstructor::SetConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Set.as_string(), *realm.intrinsics().function_prototype())
{
}

ThrowCompletionOr<void> SetConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    MUST_OR_THROW_OOM(NativeFunction::initialize(realm));

    // 24.2.2.1 Set.prototype, https://tc39.es/ecma262/#sec-set.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().set_prototype(), 0);

    define_native_accessor(realm, *vm.well_known_symbol_species(), symbol_species_getter, {}, Attribute::Configurable);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);

    return {};
}

// 24.2.1.1 Set ( [ iterable ] ), https://tc39.es/ecma262/#sec-set-iterable
ThrowCompletionOr<Value> SetConstructor::call()
{
    auto& vm = this->vm();
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.Set);
}

// 24.2.1.1 Set ( [ iterable ] ), https://tc39.es/ecma262/#sec-set-iterable
ThrowCompletionOr<NonnullGCPtr<Object>> SetConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto set = TRY(ordinary_create_from_constructor<Set>(vm, new_target, &Intrinsics::set_prototype));

    if (vm.argument(0).is_nullish())
        return set;

    auto adder = TRY(set->get(vm.names.add));
    if (!adder.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, "'add' property of Set");

    (void)TRY(get_iterator_values(vm, vm.argument(0), [&](Value iterator_value) -> Optional<Completion> {
        TRY(JS::call(vm, adder.as_function(), set, iterator_value));
        return {};
    }));

    return set;
}

// 24.2.2.2 get Set [ @@species ], https://tc39.es/ecma262/#sec-get-set-@@species
JS_DEFINE_NATIVE_FUNCTION(SetConstructor::symbol_species_getter)
{
    return vm.this_value();
}

}
