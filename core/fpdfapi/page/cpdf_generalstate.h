// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_GENERALSTATE_H_
#define CORE_FPDFAPI_PAGE_CPDF_GENERALSTATE_H_

#include <vector>

#include "constants/transparency.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/shared_copy_on_write.h"
#include "core/fxcrt/span.h"
#include "core/fxge/dib/fx_dib.h"

class CPDF_Dictionary;
class CPDF_Object;
class CPDF_TransferFunc;

class CPDF_GeneralState {
 public:
  CPDF_GeneralState();
  CPDF_GeneralState(const CPDF_GeneralState& that);
  ~CPDF_GeneralState();

  void Emplace() { ref_.Emplace(); }
  bool HasRef() const { return !!ref_; }

  void SetRenderIntent(const ByteString& ri);

  ByteString GetBlendMode() const;
  BlendMode GetBlendType() const;
  void SetBlendType(BlendMode type);

  float GetFillAlpha() const;
  void SetFillAlpha(float alpha);

  float GetStrokeAlpha() const;
  void SetStrokeAlpha(float alpha);

  RetainPtr<const CPDF_Dictionary> GetSoftMask() const;
  RetainPtr<CPDF_Dictionary> GetMutableSoftMask();
  void SetSoftMask(RetainPtr<CPDF_Dictionary> dict);

  RetainPtr<const CPDF_Object> GetTR() const;
  void SetTR(RetainPtr<const CPDF_Object> pObject);

  RetainPtr<CPDF_TransferFunc> GetTransferFunc() const;
  void SetTransferFunc(RetainPtr<CPDF_TransferFunc> pFunc);

  void SetBlendMode(const ByteString& mode);

  const CFX_Matrix* GetSMaskMatrix() const;
  void SetSMaskMatrix(const CFX_Matrix& matrix);

  bool GetFillOP() const;
  void SetFillOP(bool op);

  bool GetStrokeOP() const;
  void SetStrokeOP(bool op);

  int GetOPMode() const;
  void SetOPMode(int mode);

  void SetBG(RetainPtr<const CPDF_Object> pObject);
  void SetUCR(RetainPtr<const CPDF_Object> pObject);
  void SetHT(RetainPtr<const CPDF_Object> pObject);

  float GetFlatness() const;
  void SetFlatness(float flatness);
  void SetSmoothness(float smoothness);

  bool GetStrokeAdjust() const;
  void SetStrokeAdjust(bool adjust);

  void SetAlphaSource(bool source);
  void SetTextKnockout(bool knockout);

  // Records that the graphics state parameter dictionary `dict`, which is
  // the ExtGState resource `name`, was applied to this state. Applying the
  // recorded dictionaries in order to the initial graphics state gives this
  // state again, so a dictionary is dropped once later ones give every
  // parameter it sets a new value.
  void AppendGraphicsResource(ByteString name, RetainPtr<CPDF_Dictionary> dict);

  // Sets the names of the ExtGState resources that apply to this state:
  // one for each of `dicts`, in the same order, followed by the names of
  // the resources that are not recorded as dictionaries.
  void SetGraphicsResources(std::vector<ByteString> names,
                            std::vector<RetainPtr<CPDF_Dictionary>> dicts);
  pdfium::span<const ByteString> GetGraphicsResourceNames() const;
  pdfium::span<const RetainPtr<CPDF_Dictionary>> GetGraphicsResourceDicts()
      const;

 private:
  class StateData final : public Retainable {
   public:
    CONSTRUCT_VIA_MAKE_RETAIN;

    RetainPtr<StateData> Clone() const;

    ByteString blend_mode_ = pdfium::transparency::kNormal;
    BlendMode blend_type_ = BlendMode::kNormal;
    RetainPtr<CPDF_Dictionary> soft_mask_;
    CFX_Matrix smask_matrix_;
    float stroke_alpha_ = 1.0f;
    float fill_alpha_ = 1.0f;
    RetainPtr<const CPDF_Object> tr_;
    RetainPtr<CPDF_TransferFunc> transfer_func_;
    int render_intent_ = 0;
    bool stroke_adjust_ = false;
    bool alpha_source_ = false;
    bool text_knockout_ = false;
    bool stroke_op_ = false;
    bool fill_op_ = false;
    int opmode_ = 0;
    RetainPtr<const CPDF_Object> bg_;
    RetainPtr<const CPDF_Object> ucr_;
    RetainPtr<const CPDF_Object> ht_;
    float flatness_ = 1.0f;
    float smoothness_ = 0.0f;
    // The resource names of the graphics states that apply to this object.
    std::vector<ByteString> graphics_resource_names_;
    // The dictionaries of the first entries of `graphics_resource_names_`.
    std::vector<RetainPtr<CPDF_Dictionary>> graphics_resource_dicts_;

   private:
    StateData();
    StateData(const StateData& that);
    ~StateData() override;
  };

  SharedCopyOnWrite<StateData> ref_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_GENERALSTATE_H_
