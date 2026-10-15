# 取真數據對拍樣本：Switch 包（RomFS，塊線性）與 PC 包（已提取原件，線性）同名 .texture 各一份。
# 用法：fetch_samples.py <輸出目錄>   → <輸出目錄>/{dm,pc}/<序號>.texture + list.tsv（序號 路徑 說明）
# 選樣：覆蓋 RGBA8 / BC1 / BC3 / BC4，大小從 32² 到 2048²、含非方形；同分辨率的逐字節比，降分辨率的比解碼 PSNR。
# Fetch real-data samples for cross-checking: one copy each of the same-named .texture from the Switch package (RomFS, block-linear) and the PC package (extracted originals, linear).
# Usage: fetch_samples.py <output dir>   → <output dir>/{dm,pc}/<index>.texture + list.tsv (index, path, note)
# Sample selection: covers RGBA8 / BC1 / BC3 / BC4, sizes from 32² to 2048², including non-square; same-resolution ones are compared byte for byte, reduced-resolution ones by decoded PSNR.
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../../tools'))
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # 工作區根目錄（遊戲文件、產物、運行現場都在它下面） | Workspace root (game files, build outputs and run artifacts are all under it)
os.environ.setdefault('DMX_ROOT', os.path.join(DM_ROOT, 'data/switch/3.3.94012/Update/RomFS/data'))
import dmx
PC = os.path.join(DM_ROOT, 'data/assets-src/raw')
SAMPLES = [
    ('hd/vfx/textures/gradient/T_FX_perlin_wipe_03.texture', 'RGBA8 256² 同分辨率'),
    ('hd/vfx/textures/gradient/T_FX_portal_gradient.texture', 'RGBA8 256×16 非方形 同分辨率'),
    ('hd/env/texture/terrain/path/066_PATH.texture', 'BC4 32² 同分辨率'),
    ('hd/debug/wasted_mip_size_2048.texture', 'BC3 sRGB 2048² 12 級 同分辨率'),
    ('hd/items/armor/armor/plate_mail/textures/armor_plate_mail_alb.texture', 'BC3 sRGB 256²（PC 1024²）降分辨率'),
    ('hd/env/texture/global/flow/act4/lava/lava_lavaNS_FLOW.texture', 'BC1 64²（PC 256²）降分辨率'),
    ('hd/character/enemy/fingermage1/Textures/fingermage_KTINT.texture', 'BC3 256²（PC 1024²）降分辨率'),
]
out = sys.argv[1]
os.makedirs(f'{out}/dm', exist_ok=True); os.makedirs(f'{out}/pc', exist_ok=True)
with open(f'{out}/list.tsv', 'w') as f:
    for i, (p, why) in enumerate(SAMPLES):
        open(f'{out}/dm/{i}.texture', 'wb').write(dmx.get('data:data/' + p))
        open(f'{out}/pc/{i}.texture', 'wb').write(open(os.path.join(PC, p), 'rb').read())
        f.write(f'{i}\t{p}\t{why}\n')
print(len(SAMPLES), '對樣本 →', out)
