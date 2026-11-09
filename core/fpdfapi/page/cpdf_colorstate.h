// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_COLORSTATE_H_
#define CORE_FPDFAPI_PAGE_CPDF_COLORSTATE_H_

#include <optional>
#include <vector>

#include "core/fpdfapi/page/cpdf_color.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/shared_copy_on_write.h"
#include "core/fxcrt/span.h"
#include "core/fxge/dib/fx_dib.h"

class CPDF_ColorSpace;
class CPDF_Pattern;

class CPDF_ColorState {
 public:
  CPDF_ColorState();
  CPDF_ColorState(const CPDF_ColorState& that);
  ~CPDF_ColorState();

  void Emplace();
  void SetDefault();

  FX_COLORREF GetFillColorRef() const;
  void SetFillColorRef(FX_COLORREF colorref);

  FX_COLORREF GetStrokeColorRef() const;
  void SetStrokeColorRef(FX_COLORREF colorref);

  const CPDF_Color* GetFillColor() const;
  CPDF_Color* GetMutableFillColor();
  bool HasFillColor() const;

  const CPDF_Color* GetStrokeColor() const;
  CPDF_Color* GetMutableStrokeColor();
  bool HasStrokeColor() const;

  // `colorspace_name` is the name used by the cs/CS operator that selected
  // `colorspace`, if any. It allows callers to refer to the same color space
  // resource when writing the color back out.
  void SetFillColor(RetainPtr<CPDF_ColorSpace> colorspace,
                    std::vector<float> values,
                    ByteString colorspace_name = ByteString());
  void SetStrokeColor(RetainPtr<CPDF_ColorSpace> colorspace,
                      std::vector<float> values,
                      ByteString colorspace_name = ByteString());
  void SetFillPattern(RetainPtr<CPDF_Pattern> pattern,
                      pdfium::span<float> values,
                      ByteString colorspace_name = ByteString());
  void SetStrokePattern(RetainPtr<CPDF_Pattern> pattern,
                        pdfium::span<float> values,
                        ByteString colorspace_name = ByteString());
  // Returns the name of the color space resource that selected the fill color
  // space, or an empty string if the fill color space did not come from one.
  ByteString GetFillColorSpaceName() const;
  ByteString GetStrokeColorSpaceName() const;
  void SetFillColorSpaceName(ByteString name);
  void SetStrokeColorSpaceName(ByteString name);

  bool HasRef() const { return !!ref_; }

 private:
  class ColorData final : public Retainable {
   public:
    CONSTRUCT_VIA_MAKE_RETAIN;

    RetainPtr<ColorData> Clone() const;

    void SetDefault();

    FX_COLORREF fill_color_ref_ = 0;
    FX_COLORREF stroke_color_ref_ = 0;
    CPDF_Color fill_color_;
    CPDF_Color stroke_color_;
    ByteString fill_color_space_name_;
    ByteString stroke_color_space_name_;

   private:
    ColorData();
    ColorData(const ColorData& src);
    ~ColorData() override;
  };

  std::optional<FX_COLORREF> SetColor(RetainPtr<CPDF_ColorSpace> colorspace,
                                      std::vector<float> values,
                                      CPDF_Color& color,
                                      ByteString* color_space_name);
  FX_COLORREF SetPattern(RetainPtr<CPDF_Pattern> pattern,
                         pdfium::span<float> values,
                         CPDF_Color& color,
                         ByteString* color_space_name);

  SharedCopyOnWrite<ColorData> ref_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_COLORSTATE_H_
