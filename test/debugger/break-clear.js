// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the LICENSE
// file in the root directory of this source tree.
//
// RUN: %hdb %s < %s.debug | %FileCheck --match-full-lines %s
// REQUIRES: debugger

print('break-clear');
// CHECK-LABEL: break-clear

function foo() {
  print('first');
  print('second');
  print('third');
}

function bar() {
  debugger;
  foo();
}

bar();
// CHECK-NEXT: Break on 'debugger' statement in bar: {{.*}}:19:3
// CHECK-NEXT: Set breakpoint 1 at {{.+}}:13:3
// CHECK-NEXT: Set breakpoint 2 at {{.+}}:14:3
// CHECK-NEXT: Set breakpoint 3 at {{.+}}:15:3
// CHECK-NEXT: Continuing execution
// CHECK-NEXT: Break on breakpoint 1 in foo: {{.*}}:13:3
// CHECK-NEXT: Deleted all breakpoints
// CHECK-NEXT: Continuing execution
// CHECK-NEXT: first
// CHECK-NEXT: second
// CHECK-NEXT: third
