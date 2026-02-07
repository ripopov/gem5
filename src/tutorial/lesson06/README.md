# Lesson 6: C++ Hierarchical Modeling

This lesson introduces hierarchical modeling in gem5: building a larger
component by composing multiple smaller `ClockedObject` instances with clear
responsibilities and interfaces.

## Core vocabulary

- **Hierarchy**: a parent object composed of child objects with explicit roles.
- **Composition boundary**: the interface (methods, ports, or events) between
  parent and child behavior.
- **Local clock semantics**: each child follows a clock domain, but all events
  still execute on the shared global tick timeline.
- **Aggregation logic**: parent-level policy that coordinates child-level
  mechanisms.

## Problem statement

As simulator models grow, a single "everything in one class" `ClockedObject`
quickly becomes hard to understand, test, and extend. Timing behavior, state
management, and communication logic get tightly coupled in one place.

The challenge is to design a hierarchy of clocked components that:

1. Splits behavior into meaningful child objects (pipeline stages, schedulers,
   queues, controllers, etc.).
2. Preserves clear clock-domain semantics across parent and children.
3. Defines explicit interfaces for coordination instead of hidden shared state.
4. Supports incremental testing at both unit and integration levels.
5. Scales to realistic system complexity without becoming brittle.

This lesson will focus on turning that design problem into a repeatable
modeling approach for gem5.

## Status

Problem statement only. Implementation and runnable code examples will be
added in a later change.
