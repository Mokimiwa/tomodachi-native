# TBDR 可證省的寫出：DM_NVN_PASSDUMP 兩幀數據流。幀 A 的每個附件寫出（Store），若在該貼圖下次被清除之前沒有任何讀取
# （採樣 R、作為附件讀入 load、出圖 X），則可證為不需要（DontCare）。附件按（貼圖, 級, 層）區分：清別的級 / 層不算清它；
# 採樣 R 只記整張貼圖，按讀取算（保守）。拷貝 / 清貼圖不在轉儲裡，此處看不到，結果是上限，以運行時 DM_NVN_STORESCAN 的 Z 行為準。前瞻到幀 B 結束，跨幀歷史讀取算「需要」。讀入不判（需證全覆蓋）。
# Provably skippable TBDR stores: two frames of the DM_NVN_PASSDUMP data stream. Each attachment store (Store) in frame A is provably unnecessary (DontCare) if nothing reads it before that texture is next cleared
# (sample R, load as an attachment, present X). Attachments are distinguished by (texture, level, layer): clearing another level / layer does not count as clearing it;
# sample R only records the whole texture and is counted as a read (conservative). Copies / texture clears are not in the dump and cannot be seen here, so the result is an upper bound; the Z rows of runtime DM_NVN_STORESCAN are authoritative. Lookahead runs to the end of frame B; cross-frame history reads count as "needed". Loads are not judged (that requires proving full coverage).
import re, sys, collections
L=[l.split(None,2)[2].strip() if False else l.strip() for l in open(sys.argv[1])]
BPP={70:4,71:4,80:4,81:4,90:4,92:4,93:4,115:8,125:16,55:4,25:2,65:4,105:4,103:4,252:4,250:2,260:5,255:4,10:1,20:2,30:2,60:4}
ev=[]   # (frame, kind, pass, ser, load|clear, bytes, fmt, w, h, slot, (lv, sl))
for l in L:
    m=re.match(r'\[pass\] F(\d+) P (\d+) (c\d+|d) ser=(\d+) fmt=(\d+) (\d+)x(\d+)(?:.*? lv(\d+) sl(\d+))?.*?(clear|load|dontcare)$',l)
    if m:
        f,p,slot,s,fmt,w,h,lv,sl,act=m.groups(); fmt=int(fmt); lv=int(lv or 0); sl=int(sl or 0); w=max(int(w)>>lv,1); h=max(int(h)>>lv,1)
        ev.append((int(f),'P',int(p),int(s),act,int(w)*int(h)*BPP.get(fmt,4),fmt,int(w),int(h),slot,(lv,sl))); continue
    m=re.match(r'\[pass\] F(\d+) R (\d+) st\d+ ser=(\d+)',l)
    if m: ev.append((int(m.group(1)),'R',int(m.group(2)),int(m.group(3)),'',0,0,0,0,'',None)); continue
    m=re.match(r'\[pass\] F(\d+) X ser=(\d+)',l)
    if m: ev.append((int(m.group(1)),'X',-1,int(m.group(2)),'',0,0,0,0,'',None))
FA=min(e[0] for e in ev)
tot=0; save=0; nst=0; nsave=0; rows=collections.Counter(); load_b=0
for i,e in enumerate(ev):
    if e[0]!=FA or e[1]!='P': continue
    nst+=1; tot+=e[5]
    if e[4]=='load': load_b+=e[5]
    need=False
    for g in ev[i+1:]:
        if g[3]!=e[3]: continue
        if g[1]=='P' and g[2]==e[2]: continue          # 同一通道的其餘附件行 | the remaining attachment rows of the same pass
        if g[1] in ('R','X'): need=True; break
        if g[1]=='P' and g[10]!=e[10]: continue        # 同貼圖別的級 / 層作附件：與它無關 | another level / layer of the same texture used as an attachment: unrelated to it
        if g[1]=='P': need = g[4]=='load'; break      # 下次作附件：讀入則需要，清除 / 讀入可省（dontcare）則不需要 | next use is as an attachment: needed if loaded, not needed if cleared / load skippable (dontcare)
    if not need:
        save+=e[5]; nsave+=1; rows[(e[9][0],e[6],e[7],e[8])]+=e[5]
print('幀 %d：附件寫出 %d 個、%.1f MB；可證不需要 %d 個、%.1f MB（%.0f%%）；附件讀入 %.1f MB（不判）'%(FA,nst,tot/1e6,nsave,save/1e6,100*save/tot if tot else 0,load_b/1e6))
for k,v in rows.most_common(10): print('  %s fmt%d %dx%d：%.1f MB'%(k[0]=='d' and '深度' or '顏色',k[1],k[2],k[3],v/1e6))
