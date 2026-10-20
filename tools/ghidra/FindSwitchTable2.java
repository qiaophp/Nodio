// Ghidra headless script: find switch/jump tables whose targets all live
// inside one function (tight cluster), which is how a real 64-way opcode
// dispatch looks. MFC message maps point at many different functions and are
// therefore rejected.
//
//   -postScript FindSwitchTable2.java <outDir> <count> <maxSpan>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.File;
import java.io.PrintWriter;

public class FindSwitchTable2 extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 3) {
            println("usage: FindSwitchTable2 <outDir> <count> <maxSpan>");
            return;
        }
        String outDir = args[0];
        int need = Integer.parseInt(args[1].trim());
        long maxSpan = Long.parseLong(args[2].trim());
        new File(outDir).mkdirs();
        PrintWriter pw = new PrintWriter(new File(outDir, "switchtables2.txt"));

        var mem = currentProgram.getMemory();

        for (var blk : mem.getBlocks()) {
            if (!blk.isInitialized() || !blk.isRead() || blk.isExecute()) continue;
            long start = blk.getStart().getOffset();
            long end = blk.getEnd().getOffset();
            long p = start;
            while (p + (long) need * 4 <= end) {
                int ok = 0;
                long lo = Long.MAX_VALUE;
                long hi = Long.MIN_VALUE;
                long q = p;
                while (ok < need) {
                    int v;
                    try {
                        v = mem.getInt(toAddr(q));
                    } catch (Exception e) {
                        break;
                    }
                    if (v == 0) break;
                    var codeBlk = mem.getBlock(toAddr(v & 0xffffffffL));
                    if (codeBlk == null || !codeBlk.isExecute()) break;
                    long t = v & 0xffffffffL;
                    if (t < lo) lo = t;
                    if (t > hi) hi = t;
                    ok++;
                    q += 4;
                }
                if (ok == need && (hi - lo) <= maxSpan) {
                    Function fn = getFunctionContaining(toAddr(lo));
                    String owner = (fn == null) ? "<none>" : fn.getName();
                    String line = "TABLE at " + toAddr(p) + " count=" + need +
                        " span=0x" + Long.toHexString(hi - lo) + " owner=" + owner;
                    println(line);
                    pw.println(line);
                    for (int k = 0; k < need; k++) {
                        int v = mem.getInt(toAddr(p + (long) k * 4));
                        pw.println("    [" + k + "] " + toAddr(v & 0xffffffffL));
                    }
                    p += (long) need * 4;
                    continue;
                }
                p += 4;
            }
        }
        pw.close();
    }
}
