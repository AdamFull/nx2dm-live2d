// CubismIdManager, built in place of the SDK's (NxLive2D.cmake leaves that one
// out). Its table is global and grows as models load, while models on other
// threads look ids up - a motion's first update does - so every access holds
// one lock. Ids are never removed and each lives at a fixed address, so a
// handle stays valid after the lock is let go.

#include "core/foundation/threading/sync.h"

#include <Id/CubismId.hpp>
#include <Id/CubismIdManager.hpp>

namespace Live2D::Cubism::Framework {
namespace {

nx::mutex &table_lock() noexcept {
  static nx::mutex lock;
  return lock;
}

} // namespace

CubismIdManager::CubismIdManager() {}

CubismIdManager::~CubismIdManager() {
  for (csmUint32 i = 0; i < _ids.GetSize(); ++i)
    CSM_DELETE_SELF(CubismId, _ids[i]);
}

void CubismIdManager::RegisterIds(const csmChar **ids, const csmInt32 count) {
  for (csmInt32 i = 0; i < count; ++i)
    RegisterId(ids[i]);
}

void CubismIdManager::RegisterIds(const csmVector<csmString> &ids) {
  for (csmUint32 i = 0; i < ids.GetSize(); ++i)
    RegisterId(ids[i]);
}

const CubismId *CubismIdManager::GetId(const csmString &id) {
  return RegisterId(id.GetRawString());
}

const CubismId *CubismIdManager::GetId(const csmChar *id) {
  return RegisterId(id);
}

csmBool CubismIdManager::IsExist(const csmString &id) const {
  return IsExist(id.GetRawString());
}

csmBool CubismIdManager::IsExist(const csmChar *id) const {
  const nx::scoped_lock<nx::mutex> held(table_lock());
  return FindId(id) != nullptr;
}

const CubismId *CubismIdManager::RegisterId(const csmChar *id) {
  const nx::scoped_lock<nx::mutex> held(table_lock());
  if (CubismId *const found = FindId(id))
    return found;
  CubismId *const made = CSM_NEW CubismId(id);
  _ids.PushBack(made);
  return made;
}

const CubismId *CubismIdManager::RegisterId(const csmString &id) {
  return RegisterId(id.GetRawString());
}

// Callers hold table_lock().
CubismId *CubismIdManager::FindId(const csmChar *id) const {
  for (csmUint32 i = 0; i < _ids.GetSize(); ++i)
    if (_ids[i]->GetString() == id)
      return _ids[i];
  return nullptr;
}

} // namespace Live2D::Cubism::Framework
