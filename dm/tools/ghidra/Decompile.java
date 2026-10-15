// 按地址反編譯：-postScript Decompile.java <模塊偏移，十六進制，逗號分隔；前綴 @ = 在該地址（重）建函數> [映像基址，缺省 0x100000]
// Decompile functions by address: -postScript Decompile.java <hex module offsets, comma separated; prefix @ = (re)create the function at that address> [image base, default 0x100000]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;

public class Decompile extends GhidraScript {
    // 曾被當成不返回的 nn::os 導入調用之後的代碼沒反匯編：清掉調用返回覆蓋並繼續反匯編
    // Calls to nn::os imports once treated as non-returning left their fall-through undisassembled; clear the call-return override and disassemble past them
    void fixFlow(Address start) {
        for (int round = 0; round < 64; round++) {
            disassemble(start);
            boolean changed = false;
            Address end = start.add(0x20000);
            ghidra.program.model.listing.InstructionIterator it = currentProgram.getListing().getInstructions(start, true);
            while (it.hasNext()) {
                ghidra.program.model.listing.Instruction ins = it.next();
                if (ins.getAddress().compareTo(end) > 0) break;
                if (!ins.getFlowType().isCall()) continue;
                if (System.getenv("DECOMP_DEBUG") != null) println("call " + ins.getAddress() + " " + ins + " flow=" + ins.getFlowType() + " ftovr=" + ins.isFallThroughOverridden() + " ft=" + ins.getFallThrough() + " ovr=" + ins.getFlowOverride());
                Address[] fl = ins.getFlows(); if (fl.length == 0) continue;
                Function t = getFunctionAt(fl[0]); if (t == null) continue;
                Function r = t.isThunk() ? t.getThunkedFunction(true) : t;
                if (!r.getName(true).contains("nn::os::")) continue;
                if (ins.getFlowOverride() != ghidra.program.model.listing.FlowOverride.NONE) { ins.setFlowOverride(ghidra.program.model.listing.FlowOverride.NONE); changed = true; }
                if (ins.isFallThroughOverridden()) { ins.clearFallThroughOverride(); changed = true; }
                Address nx = ins.getMaxAddress().next();
                if (getInstructionAt(nx) == null) { disassemble(nx); changed = true; }
            }
            if (!changed) break;
        }
    }
    @Override public void run() throws Exception {
        String[] args = getScriptArgs();
        long base = args.length > 1 ? Long.decode(args[1]) : 0x100000L;
        // Ghidra 把部分 nn::os 導入（LockMutex）標成不返回，函數在第一次加鎖後就被截斷；撤掉這個標記
        // Ghidra marks some nn::os imports (LockMutex) as non-returning, which cuts functions short; undo that
        java.util.List<Function> all = new java.util.ArrayList<>();
        currentProgram.getFunctionManager().getFunctions(true).forEach(all::add);
        currentProgram.getFunctionManager().getExternalFunctions().forEach(all::add);
        for (Function g : all) {
            String n = g.getName(true);
            if (g.hasNoReturn() && (g.isThunk() || g.isExternal()) && !n.matches("(?i).*(abort|terminate|throw|exit|panic|unwind).*")) g.setNoReturn(false);
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String a : args[0].split(",")) {
            // "@偏移"：沒有函數包含它時在此建函數（只經虛表到達的代碼）
            // "@off" = create a function starting at off when none contains it (code reached only through vtables)
            boolean mk = a.trim().startsWith("@");
            Address addr = toAddr(base + Long.decode(a.trim().replace("@", "")));
            Function f = getFunctionContaining(addr);
            // "@偏移" 正是已有函數的入口：重建它（函數體可能在誤標不返回的調用處被截斷）
            // "@off" on an existing function: rebuild it, since its body may have been cut at a call wrongly marked non-returning
            if (mk && f != null && f.getEntryPoint().equals(addr)) { removeFunction(f); f = null; }
            if (f == null && mk) { fixFlow(addr); f = createFunction(addr, null); }
            if (f == null) { println("NO FUNCTION at " + a); continue; }
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            println("===== " + a + " in " + f.getName() + " @" + f.getEntryPoint() + " =====");
            println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("decompile failed: " + r.getErrorMessage()));
        }
        di.dispose();
    }
}
