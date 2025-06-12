# AK smart pointers

## Introduction

There are three main C++ smart pointer types used in Ladybird. Each type describes the ownership (or lack thereof) of the pointee.

The reason for using these pointers is to make it explicit through code who owns which resources, and how ownership is transferred. They also serve as a guard against memory leaks and use-after-free bugs.

## OwnPtr\<T\> and NonnullOwnPtr\<T\>

`OwnPtr` is used for single-owner objects. An object held in an `OwnPtr` is owned by that `OwnPtr`, and not by anybody else.

This means that the `OwnPtr` is responsible for deleting the pointee when the `OwnPtr` goes out of scope.

These pointers cannot be copied. Transferring ownership is done by moving the pointer.

`NonnullOwnPtr` is a special variant of `OwnPtr` with one additional property: it cannot be null. `NonnullOwnPtr` is suitable as a return type from functions that are guaranteed to never return null, and as an argument type where ownership is transferred, and the argument may not be null. In other words, if `OwnPtr` is "\*", then `NonnullOwnPtr` is "&".

Note: A `NonnullOwnPtr` can be assigned to an `OwnPtr` but not vice versa. To transform an known-non-null `OwnPtr` into a `NonnullOwnPtr`, use `OwnPtr::release_nonnull()`.

### Construction using helper functions

There is a `make<T>()` helper that constructs a new object and returns it wrapped in a `NonnullOwnPtr`. All arguments passed to it are forwarded to `T`'s constructor. If it fails to allocate heap memory for the object, it terminates the program.

```cpp
{
    NonnullOwnPtr<Foo> my_object = make<Foo>();
    my_object->do_stuff();
    // my_object goes out of scope here, and the Foo will be deleted.
}
```

The `try_make<T>()` helper attempts to construct a new object wrapped in an `ErrorOr<NonnullOwnPtr<T>>`. All arguments passed to it are forwarded to `T`'s constructor. In case of allocation failure, an ENOMEM error is returned. This allows the calling code to handle allocation failure as it wishes.

```cpp
auto my_object_or_error = try_make<Foo>();
if (my_object_or_error.is_error()) {
    // handle allocation failure...
}
auto my_object = my_object_or_error.release_value();
my_object->do_stuff();
```

Note: Objects constructed using `try_make<T>()` should only be dereferenced after a null check.

### Manual construction

The helper functions cannot access private constructors, so in some cases, smart pointers need to be created manually. This is done by "adopting" a raw pointer, which moves its ownership to the smart pointer. Dereferencing the raw pointer or calling its destructor afterwards can cause undefined behavior.

Known non-null pointers can be turned into a `NonnullOwnPtr` by the global `adopt_own()` function.

```cpp
NonnullOwnPtr<Foo> my_object = adopt_own(*new Foo);
```

It is safe to immediately dereference this raw pointer, as the normal `new` expression cannot return a null pointer.

Any (possibly null) pointer to `T` can be turned into an `OwnPtr<T>` by the global `adopt_own_if_nonnull()` function.

```cpp
OwnPtr<Foo> my_object = adopt_own_if_nonnull(new (nothrow) Foo);
```

In this case, the *non-throwing* `new` should be used to construct the raw pointer, which returns null if the allocation fails, instead of aborting the program.

**Note:** Always prefer the helper functions to manual construction.

## RefPtr\<T\> and NonnullRefPtr\<T\>

`RefPtr` is used for multiple-owner objects. An object held by a `RefPtr` is owned together by every pointer pointing to that object.

Shared ownership is implemented via reference counting.

`NonnullRefPtr` is a special variant of `RefPtr` with one additional property: it cannot be null. `NonnullRefPtr` is suitable as a return type from functions that are guaranteed to never return null, and as an argument type where the argument may not be null. In other words, if `RefPtr` is "\*", then `NonnullRefPtr` is "&".

Objects can only be held by `RefPtr` if they meet certain criteria. Specifically, they need to implement the functions `ref()` and `unref()`.

To make a class `T` reference-counted, you can simply make it inherit from `RefCounted<T>`. This will add all the necessary pieces to `T`.

```cpp
class Bar : public RefCounted<Bar> {
    ...
};
```

Note: A `NonnullRefPtr` can be assigned to a `RefPtr` but not vice versa. To transform an known-non-null `RefPtr` into a `NonnullRefPtr`, either use `RefPtr::release_nonnull()` or simply dereference the `RefPtr` using its `operator*`.

### Construction using helper functions

There is a `make_ref_counted<T>()` global helper function that constructs a new object and returns it wrapped in a `NonnullRefPtr`. All arguments passed to it are forwarded to `T`'s constructor. If memory cannot be allocated for the object, the program is terminated.

```cpp
NonnullRefPtr<Bar> our_object = make_ref_counted<Bar>();
NonnullRefPtr<Bar> another_owner = our_object;
```


The `try_make_ref_counted<T>()` function constructs an object wrapped in `ErrorOr<NonnullRefPtr<T>>` which may be an error if the allocation does not succeed. This allows the calling code to handle allocation failure as it wishes. All arguments passed to it are forwarded to `T`'s constructor.

```cpp
auto our_object_or_error = try_make_ref_counted<Bar>();
if (our_object_or_error.is_error()) {
    // handle allocation failure...
}
NonnullRefPtr<Bar> our_object = our_object_or_error.release_value();
RefPtr<Bar> another_owner = our_object;
```

In the above examples, the Bar object will only be deleted once both `our_object` and `another_owner` are gone.

### Manual construction

The helper functions cannot access private constructors, so in some cases, objects need to be manually wrapped into smart pointers. When constructing an object that derives from `RefCounted`, the reference count starts out at 1 (since 0 would mean that the object has no owners and should be deleted). The object must therefore be "adopted" by someone who takes responsibility of that 1. The raw pointer must not be used after its ownership is transferred to the smart pointer.

A known non-null raw pointer can be turned into a `NonnullRefPtr` by the global `adopt_ref()` function.

```cpp
NonnullRefPtr<Bar> our_object = adopt_ref(*new Bar);
```

Note: It is safe to immediately dereference this raw pointer, as the normal `new` expression cannot return a null pointer.

Any (possibly null) pointer to a reference-counted object can can be turned into a `RefPtr` by the global `adopt_ref_if_nonnull()` function.

```cpp
RefPtr<Bar> our_object = adopt_ref_if_nonnull(new (nothrow) Bar);
```
In this case, the *non-throwing* `new` should be used to construct the raw pointer, which returns null if the allocation fails, instead of aborting the program.

**Note:** Always prefer the helper functions to manual construction.

## WeakPtr\<T\>

`WeakPtr` is used for objects that somebody else owns. When the pointee of a `WeakPtr` is deleted, the `WeakPtr` will magically become null.

Behind the scenes, this is implemented using the `Weakable` template. If you want to make it possible for a class `T` to be weakly-pointed-to, have it inherit from `Weakable<T>`.

To create a `WeakPtr` to a weakable object, use `make_weak_ptr()`:

```cpp
class Baz : public Weakable<Baz> {
    ....
};

WeakPtr<Baz> a_baz;
{
    NonnullOwnPtr<Baz> my_baz = make<Baz>();
    a_baz = my_baz->make_weak_ptr();
    // a_baz now points to my_baz
}
// a_baz is now null, since my_baz went out of scope.
```
