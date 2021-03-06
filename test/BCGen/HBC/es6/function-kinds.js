// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the LICENSE
// file in the root directory of this source tree.
//
// RUN: %hermes -Xflow-parser -target=HBC -dump-bytecode %s | %FileCheck --match-full-lines %s
// REQUIRES: flowparser

function normal() {}
//CHECK-LABEL:Function<normal>(1 params, {{.*}}):

var arrow1 = () => 10;
//CHECK-LABEL:NCFunction<arrow1>(1 params, {{.*}}):
