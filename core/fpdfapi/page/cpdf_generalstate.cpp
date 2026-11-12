// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/page/cpdf_generalstate.h"

#include <set>
#include <utility>

#include "core/fpdfapi/page/cpdf_transferfunc.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/containers/contains.h"

namespace {

int RI_StringToId(const ByteString& ri) {
  uint32_t id = ri.GetID();
  if (id == FXBSTR_ID('A', 'b', 's', 'o')) {
    return 1;
  }

  if (id == FXBSTR_ID('S', 'a', 't', 'u')) {
    return 2;
  }

  if (id == FXBSTR_ID('P', 'e', 'r', 'c')) {
    return 3;
  }

  return 0;
}

BlendMode GetBlendTypeInternal(const ByteString& mode) {
  switch (mode.GetID()) {
    case FXBSTR_ID('N', 'o', 'r', 'm'):
    case FXBSTR_ID('C', 'o', 'm', 'p'):
      return BlendMode::kNormal;
    case FXBSTR_ID('M', 'u', 'l', 't'):
      return BlendMode::kMultiply;
    case FXBSTR_ID('S', 'c', 'r', 'e'):
      return BlendMode::kScreen;
    case FXBSTR_ID('O', 'v', 'e', 'r'):
      return BlendMode::kOverlay;
    case FXBSTR_ID('D', 'a', 'r', 'k'):
      return BlendMode::kDarken;
    case FXBSTR_ID('L', 'i', 'g', 'h'):
      return BlendMode::kLighten;
    case FXBSTR_ID('C', 'o', 'l', 'o'):
      if (mode.GetLength() == 10) {
        return BlendMode::kColorDodge;
      }
      if (mode.GetLength() == 9) {
        return BlendMode::kColorBurn;
      }
      return BlendMode::kColor;
    case FXBSTR_ID('H', 'a', 'r', 'd'):
      return BlendMode::kHardLight;
    case FXBSTR_ID('S', 'o', 'f', 't'):
      return BlendMode::kSoftLight;
    case FXBSTR_ID('D', 'i', 'f', 'f'):
      return BlendMode::kDifference;
    case FXBSTR_ID('E', 'x', 'c', 'l'):
      return BlendMode::kExclusion;
    case FXBSTR_ID('H', 'u', 'e', 0):
      return BlendMode::kHue;
    case FXBSTR_ID('S', 'a', 't', 'u'):
      return BlendMode::kSaturation;
    case FXBSTR_ID('L', 'u', 'm', 'i'):
      return BlendMode::kLuminosity;
  }
  return BlendMode::kNormal;
}

// Adds the keys of the graphics state parameter dictionary `dict` that give
// a parameter a new value when the dictionary is applied. These are the
// entries CPDF_AllStates::ProcessExtGS() does not skip, and the entries it
// does not know, which other PDF consumers may.
void AddKeysThatTakeEffect(const CPDF_Dictionary* dict,
                           std::set<ByteString>* keys) {
  CPDF_DictionaryLocker locker(dict);
  for (const auto& it : locker) {
    RetainPtr<const CPDF_Object> value = it.second->GetDirect();
    if (!value) {
      continue;
    }
    if (it.first == "D") {
      const CPDF_Array* dash = value->AsArray();
      if (!dash || !dash->GetArrayAt(0)) {
        continue;
      }
    } else if (it.first == "Font" && !value->IsArray()) {
      continue;
    }
    keys->insert(it.first);
  }
}

// Whether every entry of the graphics state parameter dictionary `dict` that
// stands for a parameter has its key in `keys`.
bool HasOnlyKeysFrom(const CPDF_Dictionary* dict,
                     const std::set<ByteString>& keys) {
  CPDF_DictionaryLocker locker(dict);
  for (const auto& it : locker) {
    if (it.first != "Type" && !pdfium::Contains(keys, it.first)) {
      return false;
    }
  }
  return true;
}

}  // namespace

CPDF_GeneralState::CPDF_GeneralState() = default;

CPDF_GeneralState::CPDF_GeneralState(const CPDF_GeneralState& that) = default;

CPDF_GeneralState::~CPDF_GeneralState() = default;

void CPDF_GeneralState::SetRenderIntent(const ByteString& ri) {
  ref_.GetPrivateCopy()->render_intent_ = RI_StringToId(ri);
}

ByteString CPDF_GeneralState::GetBlendMode() const {
  switch (GetBlendType()) {
    case BlendMode::kNormal:
      return ByteString(pdfium::transparency::kNormal);
    case BlendMode::kMultiply:
      return ByteString(pdfium::transparency::kMultiply);
    case BlendMode::kScreen:
      return ByteString(pdfium::transparency::kScreen);
    case BlendMode::kOverlay:
      return ByteString(pdfium::transparency::kOverlay);
    case BlendMode::kDarken:
      return ByteString(pdfium::transparency::kDarken);
    case BlendMode::kLighten:
      return ByteString(pdfium::transparency::kLighten);
    case BlendMode::kColorDodge:
      return ByteString(pdfium::transparency::kColorDodge);
    case BlendMode::kColorBurn:
      return ByteString(pdfium::transparency::kColorBurn);
    case BlendMode::kHardLight:
      return ByteString(pdfium::transparency::kHardLight);
    case BlendMode::kSoftLight:
      return ByteString(pdfium::transparency::kSoftLight);
    case BlendMode::kDifference:
      return ByteString(pdfium::transparency::kDifference);
    case BlendMode::kExclusion:
      return ByteString(pdfium::transparency::kExclusion);
    case BlendMode::kHue:
      return ByteString(pdfium::transparency::kHue);
    case BlendMode::kSaturation:
      return ByteString(pdfium::transparency::kSaturation);
    case BlendMode::kColor:
      return ByteString(pdfium::transparency::kColor);
    case BlendMode::kLuminosity:
      return ByteString(pdfium::transparency::kLuminosity);
  }
  return ByteString(pdfium::transparency::kNormal);
}

BlendMode CPDF_GeneralState::GetBlendType() const {
  const StateData* pData = ref_.GetObject();
  return pData ? pData->blend_type_ : BlendMode::kNormal;
}

void CPDF_GeneralState::SetBlendType(BlendMode type) {
  if (GetBlendType() != type) {
    ref_.GetPrivateCopy()->blend_type_ = type;
  }
}

float CPDF_GeneralState::GetFillAlpha() const {
  const StateData* pData = ref_.GetObject();
  return pData ? pData->fill_alpha_ : 1.0f;
}

void CPDF_GeneralState::SetFillAlpha(float alpha) {
  if (GetFillAlpha() != alpha) {
    ref_.GetPrivateCopy()->fill_alpha_ = alpha;
  }
}

float CPDF_GeneralState::GetStrokeAlpha() const {
  const StateData* pData = ref_.GetObject();
  return pData ? pData->stroke_alpha_ : 1.0f;
}

void CPDF_GeneralState::SetStrokeAlpha(float alpha) {
  if (GetStrokeAlpha() != alpha) {
    ref_.GetPrivateCopy()->stroke_alpha_ = alpha;
  }
}

RetainPtr<const CPDF_Dictionary> CPDF_GeneralState::GetSoftMask() const {
  const StateData* pData = ref_.GetObject();
  return pData ? pData->soft_mask_ : nullptr;
}

RetainPtr<CPDF_Dictionary> CPDF_GeneralState::GetMutableSoftMask() {
  const StateData* pData = ref_.GetObject();
  return pData ? pData->soft_mask_ : nullptr;
}

void CPDF_GeneralState::SetSoftMask(RetainPtr<CPDF_Dictionary> dict) {
  ref_.GetPrivateCopy()->soft_mask_ = std::move(dict);
}

RetainPtr<const CPDF_Object> CPDF_GeneralState::GetTR() const {
  const StateData* pData = ref_.GetObject();
  return pData ? pData->tr_ : nullptr;
}

void CPDF_GeneralState::SetTR(RetainPtr<const CPDF_Object> pObject) {
  ref_.GetPrivateCopy()->tr_ = std::move(pObject);
}

RetainPtr<CPDF_TransferFunc> CPDF_GeneralState::GetTransferFunc() const {
  const StateData* pData = ref_.GetObject();
  return pData ? pData->transfer_func_ : nullptr;
}

void CPDF_GeneralState::SetTransferFunc(RetainPtr<CPDF_TransferFunc> pFunc) {
  ref_.GetPrivateCopy()->transfer_func_ = std::move(pFunc);
}

void CPDF_GeneralState::SetBlendMode(const ByteString& mode) {
  StateData* pData = ref_.GetPrivateCopy();
  pData->blend_mode_ = mode;
  pData->blend_type_ = GetBlendTypeInternal(mode);
}

const CFX_Matrix* CPDF_GeneralState::GetSMaskMatrix() const {
  const StateData* pData = ref_.GetObject();
  return pData ? &pData->smask_matrix_ : nullptr;
}

void CPDF_GeneralState::SetSMaskMatrix(const CFX_Matrix& matrix) {
  ref_.GetPrivateCopy()->smask_matrix_ = matrix;
}

bool CPDF_GeneralState::GetFillOP() const {
  const StateData* pData = ref_.GetObject();
  return pData && pData->fill_op_;
}

void CPDF_GeneralState::SetFillOP(bool op) {
  ref_.GetPrivateCopy()->fill_op_ = op;
}

void CPDF_GeneralState::SetStrokeOP(bool op) {
  ref_.GetPrivateCopy()->stroke_op_ = op;
}

bool CPDF_GeneralState::GetStrokeOP() const {
  const StateData* pData = ref_.GetObject();
  return pData && pData->stroke_op_;
}

int CPDF_GeneralState::GetOPMode() const {
  return ref_.GetObject()->opmode_;
}

void CPDF_GeneralState::SetOPMode(int mode) {
  ref_.GetPrivateCopy()->opmode_ = mode;
}

void CPDF_GeneralState::SetBG(RetainPtr<const CPDF_Object> pObject) {
  ref_.GetPrivateCopy()->bg_ = std::move(pObject);
}

void CPDF_GeneralState::SetUCR(RetainPtr<const CPDF_Object> pObject) {
  ref_.GetPrivateCopy()->ucr_ = std::move(pObject);
}

void CPDF_GeneralState::SetHT(RetainPtr<const CPDF_Object> pObject) {
  ref_.GetPrivateCopy()->ht_ = std::move(pObject);
}

float CPDF_GeneralState::GetFlatness() const {
  const StateData* pData = ref_.GetObject();
  return pData ? pData->flatness_ : 1.0f;
}

void CPDF_GeneralState::SetFlatness(float flatness) {
  ref_.GetPrivateCopy()->flatness_ = flatness;
}

void CPDF_GeneralState::SetSmoothness(float smoothness) {
  ref_.GetPrivateCopy()->smoothness_ = smoothness;
}

bool CPDF_GeneralState::GetStrokeAdjust() const {
  const StateData* pData = ref_.GetObject();
  return pData && pData->stroke_adjust_;
}

void CPDF_GeneralState::SetStrokeAdjust(bool adjust) {
  ref_.GetPrivateCopy()->stroke_adjust_ = adjust;
}

void CPDF_GeneralState::SetAlphaSource(bool source) {
  ref_.GetPrivateCopy()->alpha_source_ = source;
}

void CPDF_GeneralState::SetTextKnockout(bool knockout) {
  ref_.GetPrivateCopy()->text_knockout_ = knockout;
}

void CPDF_GeneralState::AppendGraphicsResource(
    ByteString name,
    RetainPtr<CPDF_Dictionary> dict) {
  CHECK(dict);
  StateData* data = ref_.GetPrivateCopy();
  std::vector<ByteString>& names = data->graphics_resource_names_;
  std::vector<RetainPtr<CPDF_Dictionary>>& dicts =
      data->graphics_resource_dicts_;
  // Only names that have a dictionary can be weighed against `dict`.
  names.resize(dicts.size());

  // Walk back through the dictionaries applied so far. One whose keys are
  // all set again by the dictionaries after it has no effect on this state
  // anymore. This keeps the list no longer than the number of different
  // keys, however many `gs` operators a content stream has.
  std::set<ByteString> keys_set_later;
  AddKeysThatTakeEffect(dict.Get(), &keys_set_later);
  for (size_t i = dicts.size(); i-- > 0;) {
    if (HasOnlyKeysFrom(dicts[i].Get(), keys_set_later)) {
      names.erase(names.begin() + i);
      dicts.erase(dicts.begin() + i);
    } else {
      AddKeysThatTakeEffect(dicts[i].Get(), &keys_set_later);
    }
  }
  names.push_back(std::move(name));
  dicts.push_back(std::move(dict));
}

void CPDF_GeneralState::SetGraphicsResources(
    std::vector<ByteString> names,
    std::vector<RetainPtr<CPDF_Dictionary>> dicts) {
  CHECK_GE(names.size(), dicts.size());
  StateData* data = ref_.GetPrivateCopy();
  data->graphics_resource_names_ = std::move(names);
  data->graphics_resource_dicts_ = std::move(dicts);
}

pdfium::span<const ByteString> CPDF_GeneralState::GetGraphicsResourceNames()
    const {
  const StateData* data = ref_.GetObject();
  if (!data) {
    return {};
  }
  return data->graphics_resource_names_;
}

pdfium::span<const RetainPtr<CPDF_Dictionary>>
CPDF_GeneralState::GetGraphicsResourceDicts() const {
  const StateData* data = ref_.GetObject();
  if (!data) {
    return {};
  }
  return data->graphics_resource_dicts_;
}

CPDF_GeneralState::StateData::StateData() = default;

CPDF_GeneralState::StateData::StateData(const StateData& that)
    : blend_mode_(that.blend_mode_),
      blend_type_(that.blend_type_),
      soft_mask_(that.soft_mask_),
      smask_matrix_(that.smask_matrix_),
      stroke_alpha_(that.stroke_alpha_),
      fill_alpha_(that.fill_alpha_),
      tr_(that.tr_),
      transfer_func_(that.transfer_func_),
      render_intent_(that.render_intent_),
      stroke_adjust_(that.stroke_adjust_),
      alpha_source_(that.alpha_source_),
      text_knockout_(that.text_knockout_),
      stroke_op_(that.stroke_op_),
      fill_op_(that.fill_op_),
      opmode_(that.opmode_),
      bg_(that.bg_),
      ucr_(that.ucr_),
      ht_(that.ht_),
      flatness_(that.flatness_),
      smoothness_(that.smoothness_),
      graphics_resource_names_(that.graphics_resource_names_),
      graphics_resource_dicts_(that.graphics_resource_dicts_) {}

CPDF_GeneralState::StateData::~StateData() = default;

RetainPtr<CPDF_GeneralState::StateData> CPDF_GeneralState::StateData::Clone()
    const {
  return pdfium::MakeRetain<CPDF_GeneralState::StateData>(*this);
}
