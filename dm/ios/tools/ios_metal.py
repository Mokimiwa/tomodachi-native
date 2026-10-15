# iOS 著色器出包鏈的唯一編譯口徑：任意來源的 MSL（PC 鏈 / 常數補丁 / 今後 Switch 原版經 Ryujinx 翻譯）→ iphoneos metallib。
# 編譯參數只在這裡定義一次；反射 json 的 chain.metal 記錄由同一份參數生成，記錄 == 實際。
# The single compile standard for the iOS shader packaging chain: MSL from any source (PC chain / constant patches / in future, the Switch original translated via Ryujinx) → iphoneos metallib.
# Compile parameters are defined only once, here; the chain.metal record in the reflection json is generated from the same parameters, record == actual.
import subprocess
# -fmetal-math-mode=relaxed：保留快速數學的重排 / 合併，但不許假設沒有 NaN / Inf。翻譯出的著色器靠 isnan 選擇復現 Maxwell 的 NaN 語義（8337 / 12809 個用到），
# 缺省 fast 讓各 GPU 的後端編譯器可以把它們當成恆假；A20 Pro 上開場關卡的奇數幀主畫面 NaN 72 萬像素（M4 Pro 同一份 AIR 只有幾十個），黑帶 / 黑邊光暈即由此而來
# -fmetal-math-mode=relaxed: keep fast math's reassociation / contraction, but forbid assuming there are no NaN / Inf. The translated shaders reproduce Maxwell's NaN semantics with isnan selects (8337 / 12809 use them),
# and the default fast mode lets each GPU's backend compiler treat them as always false; on the A20 Pro the opening course's odd frames had 727k NaN pixels in the main image (a few dozen with the same AIR on the M4 Pro), which became the black band / black-edged glow
METAL = ['xcrun', '-sdk', 'iphoneos', 'metal', '-c', '-mios-version-min=18.0', '-fmetal-math-mode=relaxed']
METALLIB = ['xcrun', '-sdk', 'iphoneos', 'metallib']
RECORD = ' '.join(METAL) + '; ' + ' '.join(METALLIB)                                # 寫進 json chain.metal | Written into json chain.metal

def compile(msl, air, lib):
    subprocess.run([*METAL, msl, '-o', air], check=True)
    subprocess.run([*METALLIB, air, '-o', lib], check=True)
