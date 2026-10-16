# Ryujinx shader translator (vendored subset)

Three projects from Ryujinx 1.3.2, with one change: the FMZ multiply mode (`Instructions/InstEmitFloatArithmetic.cs`, where a zero factor makes the product 0 as on Maxwell): `Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator`.
The shader translation tool in `dm/gfx/tools/dmsh_xlate` references them to translate the original Switch shaders to SPIR-V.
License: MIT, see `LICENSE.txt`. Upstream: <https://github.com/Ryubing>.

Ryujinx 1.3.2 的三個項目，未做修改：`Ryujinx.Graphics.Shader`、`Ryujinx.Common`、`Spv.Generator`。
`dm/gfx/tools/dmsh_xlate` 的著色器翻譯工具引用它們，把 Switch 原版著色器翻譯成 SPIR-V。許可證 MIT，見 `LICENSE.txt`。
