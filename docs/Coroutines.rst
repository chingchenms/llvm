=====================================
Coroutines in LLVM
=====================================

.. contents::
   :local:
   :depth: 3

.. warning::
  This is a work in progress. Compatibility across LLVM releases is not 
  guaranteed.

.. Status
.. ======

.. This document describes a set of experimental extensions to LLVM. Use
.. with caution.  Because the intrinsics have experimental status,
.. compatibility across LLVM releases is not guaranteed. These intrinsics
.. are added to support C++ Coroutines (P0057_), though they are general enough 
.. to be used to implement coroutines in other languages as well.
.. as to experiment with C++ coroutine alternatives other than P0057.

.. _P0057: http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0057r4.pdf

Introduction
============

.. _coroutine handle:

LLVM coroutines are functions that have one or more `suspend points`_. 
When a suspend point is reached, the execution of a coroutine is suspended and
control is returned back to its caller. A suspended coroutine can be resumed 
to continue execution from the last suspend point or it can be destroyed. 

..  In the following example function `f` returns
    a handle to a suspended coroutine (**coroutine handle**) that can be passed 
    to `coro.resume`_ and `coro.destroy`_ intrinsics to resume and destroy the 
    coroutine respectively.

In the following example, we call function `f` (which may or may not be a 
coroutine itself) that returns a handle to a suspended coroutine 
(**coroutine handle**) that is used by `main` to resume the coroutine twice and
then destroy it:

.. code-block:: llvm

  define i32 @main() {
  entry:
    %hdl = call i8* @f(i32 4)
    call void @llvm.coro.resume(i8* %hdl)
    call void @llvm.coro.resume(i8* %hdl)
    call void @llvm.coro.destroy(i8* %hdl)
    ret i32 0
  }

.. _coroutine frame:

In addition to the function stack frame which exists when a coroutine is 
executing, there is an additional region of storage that contains objects that 
keep the coroutine state when a coroutine is suspended. This region of storage
is called **coroutine frame**. It is created when a coroutine is called and 
destroyed when a coroutine runs to completion or destroyed by a call to 
the `coro.destroy`_ intrinsic. 

An LLVM coroutine is represented as an LLVM function that has calls to
`coroutine intrinsics`_ defining the structure of the coroutine.
After lowering, a coroutine is split into several
functions that represent three different ways of how control can enter the 
coroutine: 

1. a ramp function, which represents an initial invocation of the coroutine that
   creates the coroutine frame and executes the coroutine code until it 
   encounters a suspend point or reaches the end of the function;

2. a coroutine resume function that is invoked when the coroutine is resumed;

3. a coroutine destroy function that is invoked when the coroutine is destroyed.

.. note:: Splitting out resume and destroy functions are just one of the 
   possible ways of lowering the coroutine. We chose it for initial 
   implementation as it matches closely the mental model and results in 
   reasonably nice code.

..
  This is not the only way of lowering the coroutine intrinsics. Another 
  alternative is to split the coroutine ever further into an individual functions
  for every suspend point.

Coroutines by Example
=====================

Coroutine Representation
------------------------

Let's look at an example of an LLVM coroutine with the behavior sketched
by the following pseudo-code.

.. code-block:: C++

  void *f(int n) {
     for(;;) {
       print(n++);
       <suspend> // returns a coroutine handle on first suspend
     }     
  } 

This coroutine calls some function `print` with value `n` as an argument and
suspends execution. Every time this coroutine resumes, it calls `print` again with an argument one bigger than the last time. This coroutine never completes by itself and must be destroyed explicitly. If we use this coroutine with 
a `main` shown in the previous section. It will call `print` with values 4, 5 
and 6 after which the coroutine will be destroyed.

The LLVM IR for this coroutine looks like this:

.. code-block:: llvm

  define i8* @f(i32 %n) {
  entry:
    %size = call i32 @llvm.coro.size.i32(i8* null)
    %alloc = call i8* @malloc(i32 %size)
    %hdl = call noalias i8* @llvm.coro.begin(i8* %alloc, i8* null, i32 0, i8* null, i8* null)
    br label %loop
  loop:
    %n.val = phi i32 [ %n, %entry ], [ %inc, %loop ]
    %inc = add nsw i32 %n.val, 1
    call void @print(i32 %n.val)
    %0 = call i8 @llvm.coro.suspend(token none, i1 false)
    switch i8 %0, label %suspend [i8 0, label %loop
                                  i8 1, label %cleanup]
  cleanup:
    %mem = call i8* @llvm.coro.free(i8* %hdl)
    call void @free(i8* %mem)
    br label %suspend
  suspend:
    call void @llvm.coro.end(i1 false)
    ret i8* %hdl
  }

The `entry` block establishes the coroutine frame. The `coro.size`_ intrinsic is
lowered to a constant representing the size required for the coroutine frame. 
The `coro.begin`_ intrinsic initializes the coroutine frame and returns the 
coroutine handle. The first parameter of `coro.begin` is given a block of memory 
to be used if the coroutine frame need to be allocated dynamically.

The `cleanup` block destroys the coroutine frame. The `coro.free`_ intrinsic, 
given the coroutine handle, returns a pointer of the memory block to be freed or
`null` if the coroutine frame was not allocated dynamically. The `cleanup` 
block is entered when coroutine runs to completion by itself or destroyed via
call to the `coro.destroy`_ intrinsic.

The `suspend` block contains code to be executed when coroutine runs to 
completion or suspended. The `coro.end`_ intrinsic marks the point where 
coroutine needs to return control back to the caller if it is not an initial 
invocation of the coroutine. 

The `loop` blocks represents the body of the coroutine. The `coro.suspend`_ 
intrinsic in combination with the following switch indicates what happens to 
control flow when a coroutine is suspended (default case), resumed (case 0) or 
destroyed (case 1).

Coroutine Transformation
------------------------

One of the steps of coroutine lowering is building the coroutine frame. The
def-use chains are analyzed to determine which objects need be kept alive across
suspend points. In the coroutine shown in the previous section, use of virtual register 
`%n.val` is separated from the definition by a suspend point, therefore, it 
cannot reside on the stack frame since the latter goes away once the coroutine 
is suspended and control is returned back to the caller. An i32 slot is 
allocated in the coroutine frame and `%n.val` is spilled and reloaded from that
slot as needed.

We also store addresses of the resume and destroy functions so that the 
`coro.resume` and `coro.destroy` intrinsics can resume and destroy the coroutine
when its identity cannot be determined statically at compile time. For our 
example, the coroutine frame will be:

.. code-block:: llvm

  %f.frame = type { void (%f.frame*)*, void (%f.frame*)*, i32 }

After resume and destroy parts are outlined, function `f` will contain only the 
code responsible for creation and initialization of the coroutine frame and 
execution of the coroutine until a suspend point is reached:

.. code-block:: llvm

  define i8* @f(i32 %n) {
  entry:
    %alloc = call noalias i8* @malloc(i32 24)
    %0 = call noalias i8* @llvm.coro.begin(i8* %alloc, i32 0, i8* null, i8* null)
    %frame = bitcast i8* %frame to %f.frame*
    %1 = getelementptr %f.frame, %f.frame* %frame, i32 0, i32 0
    store void (%f.frame*)* @f.resume, void (%f.frame*)** %1
    %2 = getelementptr %f.frame, %f.frame* %frame, i32 0, i32 1
    store void (%f.frame*)* @f.destroy, void (%f.frame*)** %2
   
    %inc = add nsw i32 %n, 1
    %inc.spill.addr = getelementptr inbounds %f.Frame, %f.Frame* %FramePtr, i32 0, i32 2
    store i32 %inc, i32* %inc.spill.addr
    call void @print(i32 %n)
   
    ret i8* %frame
  }

Outlined resume part of the coroutine will reside in function `f.resume`:

.. code-block:: llvm

  define internal fastcc void @f.resume(%f.frame* %frame.ptr.resume) {
  entry:
    %inc.spill.addr = getelementptr %f.frame, %f.frame* %frame.ptr.resume, i64 0, i32 2
    %inc.spill = load i32, i32* %inc.spill.addr, align 4
    %inc = add i32 %n.val, 1
    store i32 %inc, i32* %inc.spill.addr, align 4
    tail call void @print(i32 %inc)
    ret void
  }

Whereas function `f.destroy` will contain the cleanup code for the coroutine:

.. code-block:: llvm

  define internal fastcc void @f.destroy(%f.frame* %frame.ptr.destroy) {
  entry:
    %0 = bitcast %f.frame* %frame.ptr.destroy to i8*
    tail call void @free(i8* %0)
    ret void
  }

.. This transformation is performed by `coro-split` LLVM pass.

Avoiding Heap Allocations
-------------------------
 
A particular coroutine usage pattern, which is illustrated by the `main` 
function in the overview section, where a coroutine is created, manipulated and 
destroyed by the same calling function, is common for coroutines implementing
RAII idiom and is suitable for allocation elision optimization which avoid 
dynamic allocation by storing the coroutine frame as a static `alloca` in its 
caller.

If a coroutine uses allocation and deallocation functions that are known to
LLVM, unused calls to `malloc` and calls to `free` with `null` argument will be
removed as dead code. However, if custom allocation functions are used, the
`coro.alloc` and `coro.free` intrinsics can be used to enable removal of custom
allocation and deallocation code when coroutine does not require dynamic
allocation of the coroutine frame.

In the entry block, we will call `coro.alloc`_ intrinsic that will return `null`
when dynamic allocation is required, and non-null otherwise:

.. code-block:: llvm

  entry:
    %elide = call i8* @llvm.coro.alloc()
    %0 = icmp ne i8* %elide, null
    br i1 %0, label %coro.begin, label %dyn.alloc

  dyn.alloc:
    %frame.size = call i32 @llvm.coro.size()
    %alloc = call i8* @CustomAlloc(i32 %frame.size)
    br label %coro.begin

  coro.begin:
    %phi = phi i8* [ %elide, %entry ], [ %alloc, %dyn.alloc ]
    %frame = call i8* @llvm.coro.begin(i8* %phi, %elide, i32 0, i8* null, i8* null)

In the cleanup block, we will make freeing the coroutine frame conditional on
`coro.free`_ intrinsic. If allocation is elided, `coro.free`_ returns `null`
thus skipping the deallocation code:

.. code-block:: llvm

  cleanup:
    %mem = call i8* @llvm.coro.free(i8* %frame)
    %tobool = icmp ne i8* %mem, null
    br i1 %tobool, label %if.then, label %if.end
  if.then:
    call void @CustomFree(i8* %mem)
    br label %if.end
  if.end:
    ...

With allocations and deallocations represented as described as above, after
coroutine heap allocation elision optimization, the resulting main will end up 
looking just like it was when we used `malloc` and `free`:

.. code-block:: llvm

  define i32 @main() {
  entry:
    call void @print(i32 4)
    call void @print(i32 5)
    call void @print(i32 6)
    ret i32 0
  }


Multiple Suspend Points
-----------------------

Let's consider the coroutine that has more than one suspend point:

.. code-block:: C++

  void *f(int n) {
     for(;;) {
       print(n++);
       <suspend>
       print(-n);
       <suspend>
     }
  }

Matching LLVM code would look like (with the rest of the code remaining the same
as the code in the previous section):

.. code-block:: llvm

  coro.start:
      %n.val = phi i32 [ %n, %coro.begin ], [ %inc, %resume ]
      call void @print(i32 %n.val)
      %suspend1 = call i1 @llvm.coro.suspend(token none, i1 false)
      br i1 %suspend1, label %resume, label %cleanup

    resume:
      %inc = add i32 %n.val, 1
      %sub = sub nsw i32 0, %inc
      call void @print(i32 %sub)
      %suspend2 = call i1 @llvm.coro.suspend(token none, i1 false)
      br i1 %suspend2, label %coro.start, label %cleanup

In this case, the coroutine frame would include a suspend index that will indicate
at which suspend point the coroutine needs to resume. The resume function will 
use an index to jump to an appropriate basic block and will look as follows:

.. start with a switch as follows:

.. code-block:: llvm

  define internal fastcc void @f.resume(%f.frame* nocapture nonnull %frame.ptr.resume) {
  entry:
    %index.addr = getelementptr %f.frame, %f.frame* %frame.ptr.resume, i64 0, i32 2
    %index = load i32, i32* %0, align 4
    %switch = icmp eq i32 %index, 0
    br i1 %switch, label %resume, label %coro.start

  coro.start:
    ...
    br label %exit

  resume:
    ...
    br label %exit

  exit:
    %storemerge = phi i32 [ 1, %resume ], [ 0, %coro.start ]
    store i32 %storemerge, i32* %index.addr, align 4
    ret void
  }

If different cleanup code needs to get executed for different suspend points, 
a similar switch will be in the `f.destroy` function.

.. note ::

  Using suspend index in a coroutine state and having a switch in `f.resume` and
  `f.destroy` is one of the possible implementation strategies. We explored 
  another option where a distinct `f.resume1`, `f.resume2`, etc are created for
  every suspend point and instead of storing an index, the resume and destroy 
  function pointers are updated at every suspend. Early testing showed that the
  former is easier on the optimizer than the latter so it is a strategy 
  implemented at the moment.

Distinct Save and Suspend
-------------------------

In the previous example, setting a resume index (or some other state change that 
needs to happen to prepare a coroutine for resumption) happens at the same time as
a suspension of a coroutine. However, in certain cases, it is necessary to control 
when coroutine is prepared for resumption and when it is suspended.

In the following example, a coroutine represents some activity that is driven
by completions of asynchronous operations `async_op1` and `async_op2` which get
a coroutine handle as a parameter and resume the coroutine once async
operation is finished.

.. code-block:: llvm

  void g() {
     for (;;)
       if (cond()) {
          async_op1(<coroutine-handle>); // will resume once async_op1 completes
          <suspend>
          do_one();
       }
       else {
          async_op2(<coroutine-handle>); // will resume once async_op2 completes
          <suspend>
          do_two();
       }
     }
  }

In this case, coroutine should be ready for resumption prior to a call to 
`async_op1` and `async_op2`. The `coro.save`_ intrinsic is used to indicate a
point when coroutine should be ready for resumption:

.. code-block:: llvm

  if.true:
    %save1 = call token @llvm.coro.save()
    call void async_op1(i8* %frame)
    %suspend1 = call i1 @llvm.coro.suspend(token %save1, i1 false)
    br i1 %suspend1, label %resume1, label %cleanup

  if.false:
    %save2 = call token @llvm.coro.save()
    call void async_op2(i8* %frame)
    %suspend2 = call i1 @llvm.coro.suspend(token %save2, i1 false)
    br i1 %suspend2, label %resume2, label %cleanup

.. _coroutine promise:

Coroutine Promise
-----------------

A coroutine author or a frontend may designate a distinguished `alloca` that can
be used to communicate with the coroutine. This distinguished alloca is called
**coroutine promise** and is provided as a third parameter to the `coro.begin`_ 
intrinsic.

The following coroutine designates a 32 bit integer `promise` and uses it to
store the current value produced by a coroutine.

.. code-block:: llvm

  define i8* @f(i32 %n) {
  entry:
    %promise = alloca i32
    %pv = bitcast i32* %promise to i8*
    %frame.size = call i32 @llvm.coro.size()
    %alloc = call noalias i8* @malloc(i32 %frame.size)
    %frame = call i8* @llvm.coro.begin(i8* %alloc, i32 0, i8* %pv, i8* null)
    %first.return = call i1 @llvm.coro.fork()
    br i1 %first.return, label %coro.return, label %coro.start

  coro.start:
    %n.val = phi i32 [ %n, %entry ], [ %inc, %resume ]
    store i32 %n.val, i32* %promise
    %suspend = call i1 @llvm.coro.suspend2(token none, i1 false)
    br i1 %suspend, label %resume, label %cleanup

  resume:
    %inc = add i32 %n.val, 1
    br label %coro.start

  cleanup:
    %mem = call i8* @llvm.coro.free(i8* %frame)
    call void @free(i8* %mem)
    br label %coro.return

  coro.return:
    ret i8* %frame
  }

A coroutine consumer can rely on the `coro.promise`_ intrinsic to access the
coroutine promise.

.. code-block:: llvm

  define i32 @main() {
  entry:
    %hdl = call i8* @f(i32 4)
    %promise.addr = call i32* @llvm.coro.promise.p0i32(i8* %hdl)
    %val0 = load i32, i32* %promise.addr
    call void @print(i32 %val0)
    call void @llvm.coro.resume(i8* %hdl)
    %val1 = load i32, i32* %promise.addr
    call void @print(i32 %val1)
    call void @llvm.coro.resume(i8* %hdl)
    %val2 = load i32, i32* %promise.addr
    call void @print(i32 %val2)
    call void @llvm.coro.destroy(i8* %hdl)
    ret i32 0
  }

There is also an intrinsic `coro.from.promise`_ that performs a reverse
operation. Given an address of a coroutine promise, it obtains a coroutine handle. 
This intrinsic is the only mechanism for a user code outside of the coroutine 
to get access to the coroutine handle.

.. _final:
.. _final suspend:

Final Suspend
-------------

A coroutine author or a frontend may designate a particular suspend to be final,
by setting the second argument of the `coro.suspend`_ intrinsic to `true`.
Such a suspend point has two properties:

* it is possible to check whether a suspended coroutine is at the final suspend
  point via `coro.done` intrinsic;

* a resumption of a coroutine stopped at the final suspend point leads to 
  undefined behavior. The only possible action for a coroutine at a final
  suspend point is destroying it via `coro.destroy` intrinsic.

From the user perspective, final suspend point represents an idea of a coroutine
reaching the end. From the compiler perspective it is an optimization opportunity
for reducing number of resume points (and therefore switch cases) in the resume
function.

The following is an example of a function that keeps resuming the coroutine
until the final suspend point is reached after which point the coroutine is 
destroyed:

.. code-block:: llvm

  define i32 @main() {
  entry:
    %coro = call i8* @g()
    br %while.cond
  while.cond:
    %done = call i1 @llvm.coro.done(i8* %coro)
    br i1 %done, label %while.end, label %while.body
  while.body:
    call void @llvm.coro.resume(i8* %coro)
    br label %while.cond
  while.end:
    call void @llvm.coro.destroy(i8* %coro)
    ret i32 0
  }

Intrinsics
==========

Coroutine Manipulation Intrinsics
---------------------------------

Intrinsics described in this section are used to manipulate an existing
coroutine. They can be used in any function which happen to have a pointer
to a `coroutine frame`_ or a pointer to a `coroutine promise`_.

.. _coro.destroy:

'llvm.coro.destroy' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Syntax:
"""""""

::

      declare void @llvm.coro.destroy(i8* <handle>)

Overview:
"""""""""

The '``llvm.coro.destroy``' intrinsic destroys a suspended
coroutine.

Arguments:
""""""""""

The argument is a coroutine handle to a suspended coroutine.

Semantics:
""""""""""

When possible, the `coro.destroy` intrinsic is replaced with a direct call to 
the coroutine destroy function. Otherwise it is replaced with an indirect call 
based on the function pointer for the destroy function stored in the coroutine
frame. Destroying a coroutine that is not suspended leads to undefined behavior.

.. _coro.resume:

'llvm.coro.resume' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

::

      declare void @llvm.coro.resume(i8* <handle>)

Overview:
"""""""""

The '``llvm.coro.resume``' intrinsic resumes a suspended coroutine.

Arguments:
""""""""""

The argument is a handle to a suspended coroutine.

Semantics:
""""""""""

When possible, the `coro.resume` intrinsic is replaced with a direct call to the
coroutine resume function. Otherwise it is replaced with an indirect call based 
on the function pointer for the resume function stored in the coroutine frame. 
Resuming a coroutine that is not suspended leads to undefined behavior.

.. _coro.done:

'llvm.coro.done' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

::

      declare i1 @llvm.coro.done(i8* <handle>)

Overview:
"""""""""

The '``llvm.coro.done``' intrinsic checks whether a suspended coroutine is at 
the final suspend point or not.

Arguments:
""""""""""

The argument is a handle to a suspended coroutine.

Semantics:
""""""""""

Using this intrinsic on a coroutine that does not have a `final suspend`_ point 
or on a coroutine that is not suspended leads to undefined behavior.

.. _coro.promise:

'llvm.coro.promise' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

::

      declare <type>* @llvm.coro.promise.p0<type>(i8* <handle>)

Overview:
"""""""""

The '``llvm.coro.promise``' intrinsic returns a pointer to a 
`coroutine promise`_.

Arguments:
""""""""""

The argument is a handle to a coroutine.

Semantics:
""""""""""

Using this intrinsic on a coroutine that does not have a coroutine promise
leads to undefined behavior. It is possible to read and modify coroutine
promise of the coroutine which is currently executing. The coroutine author and
a coroutine user are responsible to makes sure there is no data races.

.. _coro.from.promise:

'llvm.coro.from.promise' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

::

    declare i8* @llvm.coro.from.promise.p0<type>(<type>* <handle>)

Overview:
"""""""""

The '``llvm.coro.from.promise``' intrinsic returns a coroutine
handle given the coroutine promise.

Arguments:
""""""""""

An address of a coroutine promise.

Semantics:
""""""""""

Using this intrinsic on a coroutine that does not have a coroutine promise
results in undefined behavior.

.. _coroutine intrinsics:

Coroutine Structure Intrinsics
------------------------------
Intrinsics described in this section are used within a coroutine to describe
the coroutine structure. They should not be used outside of a coroutine.

.. _coro.size:

'llvm.coro.size' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

    declare i32 @llvm.coro.size(i8*)
    declare i64 @llvm.coro.size(i8*)

Overview:
"""""""""

The '``llvm.coro.size``' intrinsic returns the number of bytes
required to store a `coroutine frame`_.

Arguments:
""""""""""

Before the coroutine frame is built, the argument must be `null`. After 
coroutine frame is built, the argument is replaced with a pointer to a global
private constant of the coroutine frame type.

Semantics:
""""""""""

The `coro.size` intrinsic is lowered to a constant representing the size of
the coroutine frame. 

.. _coro.begin:

'llvm.coro.begin' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i8* @llvm.coro.begin(i8* %mem, i32 %align, i8* %promise, i8* %fnaddr)

Overview:
"""""""""

The '``llvm.coro.begin``' intrinsic returns an address of the 
coroutine frame.

Arguments:
""""""""""

The first argument is a pointer to a block of memory in which coroutine frame
will reside. This could be the result of an allocation function or the result of
a call to a `coro.alloc`_ intrinsics representing a storage that can be used on a
frame of the calling function.

The second argument provides information on alignment of the memory returned by
the allocation function and given to `coro.begin` by the first parameter. If this
argument is 0, the memory is assumed to be aligned to 2 * sizeof(i8*).
This argument only accepts constants.

The third argument, if not `null`, designates a particular alloca instruction to
be a `coroutine promise`_.

The fourth argument is a function pointer to a coroutine itself.
If this argument is `null`, CoroEarly pass will replace it
with an address of the enclosing function. 

.. note::
  Since `coro.begin` intrinsic is not lowered until late optimizer passes, 
  `fnaddr` argument can be used to distinguish between `coro.begin` that 
  describes a structure of a pre-split coroutine or a `coro.begin` belonging to 
  a post-split coroutine that was inlined into a different function.

Semantics:
""""""""""

Depending on the alignment requirements of the objects in the coroutine frame
and/or on the codegen compactness reasons the pointer returned from `coro.begin` 
may be at offset to the `%mem` argument. (This could be beneficial if instructions
that express relative access to data can be more compactly encoded with small
positive and negative offsets).

Frontend should emit exactly one `coro.begin` intrinsic per coroutine.
It should appear prior to `coro.fork`_ intrinsic.

.. _coro.free:

'llvm.coro.free' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i8* @llvm.coro.free(i8* %frame)

Overview:
"""""""""

The '``llvm.coro.free``' intrinsic returns a pointer to a block
of memory where coroutine frame is stored or `null` if the allocation
of the coroutine frame was elided.

Arguments:
""""""""""

A pointer to the coroutine frame. This should be the same pointer that was 
returned by prior `coro.begin` call.

Example (allow heap allocation elision):
""""""""""""""""""""""""""""""""""""""""

.. code-block:: llvm

  cleanup:
    %mem = call i8* @llvm.coro.free(i8* %frame)
    %tobool = icmp ne i8* %mem, null
    br i1 %tobool, label %if.then, label %if.end

  if.then:
    call void @free(i8* %mem)
    br label %if.end

  if.end:
    ret void

Example (no heap allocation elision):
""""""""""""""""""""""""""""""""""""""""

.. code-block:: llvm

  cleanup:
    %mem = call i8* @llvm.coro.free(i8* %frame)
    call void @free(i8* %mem)
    ret void


.. _coro.alloc:

'llvm.coro.alloc' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i8* @llvm.coro.alloc()

Overview:
"""""""""

The '``llvm.coro.alloc``' intrinsic returns an address of the 
memory on the callers frame where coroutine frame of this coroutine can be 
placed and `null` otherwise.

Arguments:
""""""""""

None

Semantics:
""""""""""

If the coroutine is eligible for heap elision and the ramp function is inlined
in its caller, this intrinsic is lowered to an alloca storing the coroutine frame.
Otherwise, it is lowered to constant `null`.

Example:
""""""""""

.. code-block:: llvm

  entry:
    %elide = call i8* @llvm.coro.alloc()
    %0 = icmp ne i8* %elide, null
    br i1 %0, label %coro.begin, label %coro.alloc

  coro.alloc:
    %frame.size = call i32 @llvm.coro.size()
    %alloc = call i8* @malloc(i32 %frame.size)
    br label %coro.begin

  coro.begin:
    %phi = phi i8* [ %elide, %entry ], [ %alloc, %coro.alloc ]
    %frame = call i8* @llvm.coro.begin(i8* %phi, i32 0, i8* null, i8* null)

.. _coro.frame:

'llvm.coro.frame' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i8* @llvm.coro.frame()

Overview:
"""""""""

The '``llvm.coro.frame``' intrinsic returns an address of the 
coroutine frame.

Arguments:
""""""""""

None

Semantics:
""""""""""

This intrinsic is lowered to refer to the `coro.begin`_ instruction. This is
a frontend convenience intrinsic that makes it easier to refer to the
coroutine frame. This intrinsic is not necessary for the llvm coroutine model 
and can be removed.

.. _coro.fork:

'llvm.coro.fork' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i1 @llvm.coro.fork()

Overview:
"""""""""

The '``llvm.coro.fork``' intrinsic is used to indicate where the
control should transfer on the first suspension of the coroutine. 

Arguments:
""""""""""

None

Semantics:
""""""""""
The true branch of the the conditional branch consuming the boolean value 
returned from this intrinsic indicates where the control should transfer on
the first suspension of the coroutine.  
In the ramp function, when suspend points are lowered,  every `coro.suspend` is
replaced with a jump to the basic block designated by the true branch.

The 'coro.fork` itself is always lowered to constant `false`.

.. _coro.end:

'llvm.coro.end' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare void @llvm.coro.end()

Overview:
"""""""""

The '``llvm.coro.end``' marks the point where execution
of the resume part of the coroutine should end and control returns back to 
the caller.


Arguments:
""""""""""

None

Semantics:
""""""""""
The `coro.end`_ intrinsic is a no-op during an initial invocation of the 
coroutine. When the coroutine resumes, the intrinsic marks the point when 
coroutine need to return control back to the caller.

This intrinsic is removed by the CoroSplit pass when a coroutine is split into
the start, resume and destroy parts. In start part, the intrinsic is removed,
in resume and destroy parts, it is replaced with `ret void` instructions and
the rest of the block containing `coro.end` instruction is discarded.

In landing pads it is replaced with an appropriate instruction to unwind to 
caller.

.. _coro.suspend:
.. _suspend points:

'llvm.coro.suspend' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i1 @llvm.coro.suspend(token %save, i1 %final)

Overview:
"""""""""

The '``llvm.coro.suspend``' marks the point where execution
of the coroutine need to get suspended and control returned back to the caller.
Conditional branch consuming the result of this intrinsic marks basic blocks
where coroutine should proceed when resumed via `coro.resume` and `coro.destroy` 
intrinsics if the coroutine is suspended at this particular suspend point.

Arguments:
""""""""""

The first argument refers to a token of `coro.save` intrinsic that marks the 
point when coroutine state is prepared for suspension. If `none` token is passed,
the intrinsic behaves as if there were a `coro.save` immediately preceding
the `coro.suspend` intrinsic.

The second argument indicates whether this suspension point is `final`_.
The second argument only accepts constants. If more than one suspend point is
designated as final, the resume and destroy branches should lead to the same
basic blocks.

Semantics:
""""""""""

If a coroutine that was suspended at the suspend point marked by this intrinsic
is resumed via `coro.resume`_ the control will transfer to the basic block
marked by the true branch of the conditional branch consuming the result of the
`coro.suspend`. If it is resumed via `coro.destroy`_, it will proceed to the
basic block indicated by the false branch.

If suspend intrinsic is marked as final, it can consider the `true` branch
unreachable and can perform optimizations that can take advantage of that fact.

.. _coro.save:

'llvm.coro.save' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare token @llvm.coro.save()

Overview:
"""""""""

The '``llvm.coro.save``' marks the point where a coroutine 
is considered suspened (and thus eligible for resumption). Its return value 
should be consumed by exactly one `coro.suspend` intrinsic.

Arguments:
""""""""""

None

Semantics:
""""""""""

Whatever coroutine state changes are required to enable resumption of
the coroutine from the corresponding suspend point should be done at the point of
`coro.save` intrinsic.

Example:
""""""""

Separate save and suspend points are necessary when a coroutine is used to 
represent an asynchronous control flow driven by callbacks representing
completions of asynchronous operations.

In such a case, a coroutine should be ready for resumption prior to a call to 
`async_op` function that may trigger resumption of a coroutine from the same or
a different thread possibly prior to `async_op` call returning control back
to the coroutine:

.. code-block:: llvm

    %save = call token @llvm.coro.save()
    call void async_op(i8* %frame)
    %suspend = call i1 @llvm.coro.suspend(token %save, i1 false)
    br i1 %suspend, label %resume, label %cleanup

Coroutine Transformation Passes
===============================
CoroEarly
---------
The pass CoroEarly lowers coroutine intrinsics that hide the details of the
structure of the coroutine frame, but, otherwise not needed to be preserved to
help later coroutine passes. This pass lowers `coro.frame`_, `coro.done`_, 
`coro.promise`_ and `coro.from.promise`_ intrinsics.

CoroInline
----------
Since coroutine transformation need to be done in the IPO order and inlining
pre-split coroutine is undesirable, the CoroInline pass wraps the inliner pass
to execute coroutine and inliner passes in the following order.

#. Call sites in the function `F` are inlined as appropriate
#. CoroElide pass is run on the function `F` to see if any coroutines were 
   inlined and are eligible for coroutine frame elision optimization.
#. If function `F` is a coroutine, resume and destroy parts are extracted into
   `F.resume` and `F.destroy` functions by the CoroSplit pass. 

.. _CoroSplit:

CoroSplit
---------
The pass CoroSplit extracts resume and destroy parts into separate functions.

CoroElide
---------
The pass CoroElide examines if the inlined coroutine is eligible for heap 
allocation elision optimization. If so, it replaces `coro.alloc` intrinsic with
an address of a coroutine frame placed on its caller and replaces
`coro.free` intrinsics with `null` to remove the deallocation code. This pass
also replaces `coro.resume` and `coro.destroy` intrinsics with direct calls to
resume and destroy functions for a particular coroutine where possible.

CoroCleanup
-----------
This pass runs late to lower all coroutine related intrinsics not replaced by
earlier passes.

Areas Requiring Attention
=========================
#. Debug information is not supported at the moment.

#. A coroutine frame is bigger than it could be. Adding stack packing and stack 
   coloring like optimization on the coroutine frame will result in tighter
   coroutine frames.

#. Take advantage of the lifetime intrinsics for the data that goes into the
   coroutine frame. Leave lifetime intrinsics as is for the data that stays in
   allocas.

#. The CoroElide optimization pass relies on coroutine ramp function to be
   inlined. It would be beneficial to split the ramp function further to increase 
   the chance that it will get inlined into its caller.

#. Design a convention that would make it possible to apply coroutine heap
   elision optimization across ABI boundaries.

#. Cannot handle coroutines with inalloca parameters (used in x86 on Windows)

#. Alignment is ignored by coro.begin and coro.free intrinsics.

#. Make required changes to make sure that coroutine optimizations work with
   LTO.

#. Would coro.start be a better name than coro.fork?
