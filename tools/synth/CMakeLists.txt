# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the LICENSE
# file in the root directory of this source tree.

if(HERMESVM_SYNTH_REPLAY)
  add_llvm_tool(synth
    synth.cpp
  )
endif()

