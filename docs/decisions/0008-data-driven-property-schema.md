# 0008 — Properties are a runtime schema (`ClassInfo`) plus indexed storage (`PropertyBag`)

**Status:** accepted, 2026-09-27

## Context

§4.1 asks for "a reflection-lite system: a class declares its properties once (name, type, getter, setter,
category, default)". Clannect's object classes are not C++ classes. `ObjectCatalog` defines them as **data**
(`ObjectDef` + `PropertyDef`), and every object is one C++ `Instance` holding a `QVariantMap`. The property
system therefore has to be a runtime schema, not compile-time reflection over C++ members.

## Decision

- **`PropertyInfo`** is one declared property: name, type, default, section (the spec's "category"), choices
  and flags (ReadOnly, Hidden, Transient). It maps one-to-one onto today's `PropertyDef`. `PropertyKind::Choice`
  becomes a String property with a `choices` list. `Number` becomes `Double`. `Text` becomes `String`.
- **`ClassInfo`** is an immutable, validated list of `PropertyInfo`, with an open-addressing index keyed by
  `Name` hash. Lookup is O(1) and allocation-free, which meets the spec's requirement. `inherit()` copies a
  base class's properties, for shared GUI and part properties.
- **`PropertyBag`** holds one object's values in a vector indexed by property index:
  - An unset slot reads as the declared default. The vector is allocated on the first `set()`, so an
    untouched object costs one pointer.
  - `set()` enforces type and choices. It accepts Int for Number, because scene JSON may write `5` for `5.0`.
  - `set()` reports whether the value changed, which is what undo and change notification need.
- **Extras** are undeclared keys kept aside, so a scene saved by a newer engine round-trips without losing
  data. Only loaders write extras. A misspelt property name in code is a `NotFound` error rather than a
  silent new key, which is a bug class the `QVariantMap` bag allows today.

## Deferred

- **Native getter/setter accessors** for C++ classes (cfw-ui elements will want them). They will be added
  alongside cfw-ui (M5), when there is a real class to design them against. `PropertyInfo` can gain an
  optional accessor pair without changing `PropertyBag`.
- **Change signals** live on the owner (`Instance`), not in the bag, so the bag stays a plain container.

## Rejected

- **Keeping a string-keyed map (`FlatMap<Name, Variant>`) per instance.** It is O(log n), allocates per
  property, and gives no type enforcement.
- **Compile-time reflection with macros or code generation.** Constraint 2 forbids code generation, and
  Clannect's classes are data anyway.
