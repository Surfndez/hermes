/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "hermes/VM/RuntimeModule.h"

#include "hermes/Support/PerfSection.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/Predefined.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/RuntimeModule-inline.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"

namespace hermes {
namespace vm {

RuntimeModule::RuntimeModule(
    Runtime *runtime,
    Handle<Domain> domain,
    RuntimeModuleFlags flags,
    llvm::StringRef sourceURL)
    : runtime_(runtime),
      domain_(&runtime->getHeap(), domain),
      flags_(flags),
      sourceURL_(sourceURL) {
  assert(
      domain_.isValid() && vmisa<Domain>(domain_.unsafeGetHermesValue()) &&
      "initialized with invalid domain");
  runtime_->addRuntimeModule(this);
  Domain::addRuntimeModule(domain, runtime, this);
}

SymbolID RuntimeModule::createSymbolFromStringIDMayAllocate(
    StringID stringID,
    const StringTableEntry &entry,
    OptValue<uint32_t> mhash) {
  // Use manual pointer arithmetic to avoid out of bounds errors on empty
  // string accesses.
  auto strStorage = bcProvider_->getStringStorage();
  if (entry.isUTF16()) {
    const char16_t *s =
        (const char16_t *)(strStorage.begin() + entry.getOffset());
    UTF16Ref str{s, entry.getLength()};
    uint32_t hash = mhash ? *mhash : hashString(str);
    return mapStringMayAllocate(str, stringID, hash);
  } else {
    // ASCII.
    const char *s = strStorage.begin() + entry.getOffset();
    ASCIIRef str{s, entry.getLength()};
    uint32_t hash = mhash ? *mhash : hashString(str);
    return mapStringMayAllocate(str, stringID, hash);
  }
}

RuntimeModule::~RuntimeModule() {
  runtime_->removeRuntimeModule(this);

  // We may reference other CodeBlocks through lazy compilation, but we only
  // own the ones that reference us.
  for (auto *block : functionMap_) {
    if (block != nullptr && block->getRuntimeModule() == this) {
      delete block;
    }
  }
}

void RuntimeModule::prepareForRuntimeShutdown() {
  for (int i = 0, e = functionMap_.size(); i < e; i++) {
    if (functionMap_[i] != nullptr &&
        functionMap_[i]->getRuntimeModule() != this) {
      functionMap_[i] = nullptr;
    }
  }
}

CallResult<RuntimeModule *> RuntimeModule::create(
    Runtime *runtime,
    Handle<Domain> domain,
    std::shared_ptr<hbc::BCProvider> &&bytecode,
    RuntimeModuleFlags flags,
    llvm::StringRef sourceURL) {
  auto *result = new RuntimeModule(runtime, domain, flags, sourceURL);
  if (bytecode) {
    if (result->initializeMayAllocate(std::move(bytecode)) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return result;
}

void RuntimeModule::initializeWithoutCJSModulesMayAllocate(
    std::shared_ptr<hbc::BCProvider> &&bytecode) {
  assert(!bcProvider_ && "RuntimeModule already initialized");
  bcProvider_ = std::move(bytecode);
  importStringIDMapMayAllocate();
  initializeFunctionMap();
}

ExecutionStatus RuntimeModule::initializeMayAllocate(
    std::shared_ptr<hbc::BCProvider> &&bytecode) {
  initializeWithoutCJSModulesMayAllocate(std::move(bytecode));
  if (LLVM_UNLIKELY(importCJSModuleTable() == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

CodeBlock *RuntimeModule::getCodeBlockSlowPath(unsigned index) {
#ifndef HERMESVM_LEAN
  if (bcProvider_->isFunctionLazy(index)) {
    auto *lazyModule = RuntimeModule::createLazyModule(
        runtime_, getDomain(runtime_), this, index);
    functionMap_[index] = lazyModule->getOnlyLazyCodeBlock();
    return functionMap_[index];
  }
#endif
  functionMap_[index] = CodeBlock::createCodeBlock(
      this,
      bcProvider_->getFunctionHeader(index),
      bcProvider_->getBytecode(index),
      index);
  return functionMap_[index];
}

#ifndef HERMESVM_LEAN
RuntimeModule *RuntimeModule::createLazyModule(
    Runtime *runtime,
    Handle<Domain> domain,
    RuntimeModule *parent,
    uint32_t functionID) {
  auto RM = createUninitialized(runtime, domain);
  // Set the bcProvider's BytecodeModule to point to the parent's.
  assert(parent->isInitialized() && "Parent module must have been initialized");

  hbc::BytecodeFunction *bcFunction =
      &((hbc::BCProviderFromSrc *)parent->getBytecode())
           ->getBytecodeModule()
           ->getFunction(functionID);

  RM->bcProvider_ = hbc::BCProviderLazy::createBCProviderLazy(bcFunction);

  // We don't know which function index this block will eventually represent,
  // so just add it as 0 to ensure ownership. We'll move it later in
  // `initializeLazy`.
  RM->functionMap_.emplace_back(CodeBlock::createCodeBlock(
      RM, RM->bcProvider_->getFunctionHeader(functionID), {}, functionID));

  // The module doesn't have a string table until we've compiled the block,
  // so just add the string name as 0 in the mean time for f.name to work via
  // getLazyName(). Since it's in the stringIDMap_, it'll be correctly GC'd.
  RM->stringIDMap_.push_back(parent->getSymbolIDFromStringIDMayAllocate(
      bcFunction->getHeader().functionName));

  return RM;
}

SymbolID RuntimeModule::getLazyName() {
  assert(functionMap_.size() == 1 && "Not a lazy module?");
  assert(stringIDMap_.size() == 1 && "Missing lazy function name symbol");
  assert(this->stringIDMap_[0].isValid() && "Invalid function name symbol");
  return this->stringIDMap_[0];
}

bool RuntimeModule::getLazyNameString(Runtime *runtime, std::string &res)
    const {
  assert(functionMap_.size() == 1 && "Not a lazy module?");
  assert(stringIDMap_.size() == 1 && "Missing lazy function name symbol");
  assert(this->stringIDMap_[0].isValid() && "Invalid function name symbol");
  StringView strView = runtime->getIdentifierTable().getStringView(
      runtime, this->stringIDMap_[0]);
  if (strView.isASCII()) {
    res = std::string(strView.begin(), strView.end());
    return true;
  } else {
    return false;
  }
}

void RuntimeModule::initializeLazyMayAllocate(
    std::unique_ptr<hbc::BCProvider> bytecode) {
  // Clear the old data provider first.
  bcProvider_ = nullptr;

  // Initialize without CJS module table because this compilation is done
  // separately, and the bytecode will not contain a module table.
  initializeWithoutCJSModulesMayAllocate(std::move(bytecode));

  // createLazyCodeBlock added a single codeblock as functionMap_[0]
  assert(functionMap_[0] && "Missing first entry");

  // We should move it to the index where it's supposed to be. This ensures a
  // 1-1 relationship between codeblocks and bytecodefunctions, which the
  // debugger relies on for setting step-out breakpoints in all functions.
  if (bcProvider_->getGlobalFunctionIndex() == 0) {
    // No move needed
    return;
  }
  assert(
      !functionMap_[bcProvider_->getGlobalFunctionIndex()] &&
      "Entry point is already occupied");
  functionMap_[bcProvider_->getGlobalFunctionIndex()] = functionMap_[0];
  functionMap_[0] = nullptr;
}
#endif

void RuntimeModule::importStringIDMapMayAllocate() {
  assert(bcProvider_ && "Uninitialized RuntimeModule");
  PerfSection perf("Import String ID Map");
  GCScope scope(runtime_);

  auto strTableSize = bcProvider_->getStringCount();

  stringIDMap_.clear();

  // Populate the string ID map with empty identifiers.
  stringIDMap_.resize(strTableSize, SymbolID::empty());

  // Preallocate enough space to store all identifiers to prevent
  // unnecessary allocations.
  runtime_->getIdentifierTable().reserve(strTableSize);

  if (runtime_->getVMExperimentFlags() &
      experiments::MAdviseStringsSequential) {
    bcProvider_->adviseStringTableSequential();
  }

  if (runtime_->getVMExperimentFlags() & experiments::MAdviseStringsWillNeed) {
    bcProvider_->willNeedStringTable();
  }

  // Get the array of pre-computed translations from identifiers in the bytecode
  // to their runtime representation as SymbolIDs.
  auto kinds = bcProvider_->getStringKinds();
  auto translations = bcProvider_->getIdentifierTranslations();
  assert(
      translations.size() <= strTableSize &&
      "Should not have more strings than identifiers");
  {
    StringID strID = 0;
    uint32_t trnID = 0;

    for (auto entry : kinds) {
      switch (entry.kind()) {
        case StringKind::String:
          strID += entry.count();
          break;

        case StringKind::Identifier:
          for (uint32_t i = 0; i < entry.count(); ++i, ++strID, ++trnID) {
            createSymbolFromStringIDMayAllocate(
                strID,
                bcProvider_->getStringTableEntry(strID),
                translations[trnID]);
          }
          break;

        case StringKind::Predefined:
          for (uint32_t i = 0; i < entry.count(); ++i, ++strID, ++trnID) {
            mapPredefined(strID, translations[trnID]);
          }
          break;
      }
    }

    assert(strID == strTableSize && "Should map every string in the bytecode.");
    assert(trnID == translations.size() && "Should translate all identifiers.");
  }

  if (runtime_->getVMExperimentFlags() & experiments::MAdviseStringsRandom) {
    bcProvider_->adviseStringTableRandom();
  }

  if (strTableSize == 0) {
    // If the string table turns out to be empty,
    // we always add one empty string to it.
    // Note that this can only happen when we are creating the RuntimeModule
    // in a non-standard way, either in unit tests or the special
    // emptyCodeBlockRuntimeModule_ in Runtime where the creation happens
    // manually instead of going through bytecode module generation.
    // In those cases, functions will be created with a default nameID=0
    // without adding the name string into the string table. Hence here
    // we need to add it manually and it will have index 0.
    ASCIIRef s;
    stringIDMap_.push_back({});
    mapStringMayAllocate(s, hashString(s));
  }
}

void RuntimeModule::initializeFunctionMap() {
  assert(bcProvider_ && "Uninitialized RuntimeModule");
  assert(
      bcProvider_->getFunctionCount() >= functionMap_.size() &&
      "Unexpected size reduction. Lazy module missing functions?");
  functionMap_.resize(bcProvider_->getFunctionCount());
}

ExecutionStatus RuntimeModule::importCJSModuleTable() {
  PerfSection perf("Import CJS Module Table");
  return Domain::importCJSModuleTable(
      getDomain(runtime_), runtime_, this, bcProvider_->getCJSModuleOffset());
}

StringPrimitive *RuntimeModule::getStringPrimFromStringIDMayAllocate(
    StringID stringID) {
  return runtime_->getStringPrimFromSymbolID(
      getSymbolIDFromStringIDMayAllocate(stringID));
}

bool RuntimeModule::getStringFromStringID(StringID stringID, std::string &res) {
  auto entry = bcProvider_->getStringTableEntry(stringID);
  if (entry.isUTF16()) {
    return false;
  } else {
    // ASCII.
    auto strStorage = bcProvider_->getStringStorage();
    const char *s = strStorage.begin() + entry.getOffset();
    res = std::string{s, entry.getLength()};
    return true;
  }
}

llvm::ArrayRef<uint8_t> RuntimeModule::getRegExpBytecodeFromRegExpID(
    uint32_t regExpId) const {
  assert(
      regExpId < bcProvider_->getRegExpTable().size() && "Invalid regexp id");
  RegExpTableEntry entry = bcProvider_->getRegExpTable()[regExpId];
  return bcProvider_->getRegExpStorage().slice(entry.offset, entry.length);
}

template <typename T>
SymbolID RuntimeModule::mapStringMayAllocate(
    llvm::ArrayRef<T> str,
    StringID stringID,
    uint32_t hash) {
  // Create a SymbolID for a given string. In general a SymbolID holds onto an
  // intern'd StringPrimitive. As an optimization, if this RuntimeModule is
  // persistent, then it will not be deallocated before the Runtime, and we can
  // have the SymbolID hold a raw pointer into the storage and produce the
  // StringPrimitive when it is first required.
  SymbolID id;
  if (flags_.persistent) {
    // Registering a lazy identifier does not allocate, so we do not need a
    // GC scope.
    id = runtime_->getIdentifierTable().registerLazyIdentifier(str, hash);
  } else {
    // Accessing a symbol non-lazily may allocate in the GC heap, so add a scope
    // marker.
    GCScopeMarkerRAII scopeMarker{runtime_};
    id = *runtime_->ignoreAllocationFailure(
        runtime_->getIdentifierTable().getSymbolHandle(runtime_, str, hash));
  }
  stringIDMap_[stringID] = id;
  return id;
}

SymbolID RuntimeModule::mapPredefined(StringID stringID, uint32_t rawSymbolID) {
  SymbolID id = SymbolID::unsafeCreate(rawSymbolID);
  assert(Predefined::isPredefined(id));

  stringIDMap_[stringID] = id;
  return id;
}

void RuntimeModule::markRoots(SlotAcceptor &acceptor, bool markLongLived) {
  for (auto &it : templateMap_) {
    acceptor.acceptPtr(it.second);
  }

  if (markLongLived) {
    for (auto symbol : stringIDMap_) {
      if (symbol.isValid()) {
        acceptor.accept(symbol);
      }
    }
  }
}

void RuntimeModule::markWeakRoots(SlotAcceptor &acceptor) {
  for (auto &cbPtr : functionMap_) {
    // Only mark a CodeBlock is its non-null, and has not been scanned
    // previously in this top-level markRoots invocation.
    if (cbPtr != nullptr && cbPtr->getRuntimeModule() == this) {
      cbPtr->markCachedHiddenClasses(acceptor);
    }
  }
  for (auto &entry : objectLiteralHiddenClasses_) {
    if (entry.second) {
      acceptor.accept(reinterpret_cast<void *&>(entry.second));
    }
  }
}

void RuntimeModule::markDomainRef(GC *gc) {
  gc->markWeakRef(domain_);
}

llvm::Optional<Handle<HiddenClass>> RuntimeModule::findCachedLiteralHiddenClass(
    unsigned keyBufferIndex,
    unsigned numLiterals) const {
  if (canGenerateLiteralHiddenClassCacheKey(keyBufferIndex, numLiterals)) {
    const auto cachedHiddenClassIter = objectLiteralHiddenClasses_.find(
        getLiteralHiddenClassCacheHashKey(keyBufferIndex, numLiterals));
    if (cachedHiddenClassIter != objectLiteralHiddenClasses_.end() &&
        cachedHiddenClassIter->second != nullptr) {
      return runtime_->makeHandle(cachedHiddenClassIter->second);
    }
  }
  return llvm::None;
}

void RuntimeModule::tryCacheLiteralHiddenClass(
    unsigned keyBufferIndex,
    HiddenClass *clazz) {
  auto numLiterals = clazz->getNumProperties();
  if (canGenerateLiteralHiddenClassCacheKey(keyBufferIndex, numLiterals)) {
    assert(
        !findCachedLiteralHiddenClass(keyBufferIndex, numLiterals).hasValue() &&
        "Why are we caching an item already cached?");
    objectLiteralHiddenClasses_[getLiteralHiddenClassCacheHashKey(
        keyBufferIndex, numLiterals)] = clazz;
  }
}

size_t RuntimeModule::additionalMemorySize() const {
  size_t total = stringIDMap_.capacity() * sizeof(SymbolID) +
      functionMap_.capacity() * sizeof(CodeBlock *) +
      objectLiteralHiddenClasses_.getMemorySize() +
      templateMap_.getMemorySize();
  // Add the size of each CodeBlock
  for (const CodeBlock *cb : functionMap_) {
    // Skip the null code blocks, they are lazily inserted the first time
    // they are used.
    if (cb && cb->getRuntimeModule() == this) {
      // Only add the size of a CodeBlock if this runtime module is the owner.
      total += sizeof(CodeBlock) + cb->additionalMemorySize();
    }
  }
  return total;
}

namespace detail {

StringID mapStringMayAllocate(RuntimeModule &module, const char *str) {
  module.stringIDMap_.push_back({});
  module.mapStringMayAllocate(
      createASCIIRef(str), module.stringIDMap_.size() - 1);
  return module.stringIDMap_.size() - 1;
}

} // namespace detail

} // namespace vm

} // namespace hermes
