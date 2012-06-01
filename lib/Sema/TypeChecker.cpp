//===--- TypeChecker.cpp - Type Checking ----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for expressions, and other pieces
// that require final type checking.  If this passes a translation unit with no
// errors, then it is good to go.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/NameLookup.h"
using namespace swift;

ProtocolDecl *TypeChecker::getRangeProtocol() {
  if (!RangeProto) {
    UnqualifiedLookup Globals(Context.getIdentifier("Range"), &TU);
    if (Globals.Results.size() != 1)
      return nullptr;
    
    RangeProto = dyn_cast<ProtocolDecl>(Globals.Results.front());
  }
  
  return RangeProto;
}
