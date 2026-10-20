// Ghidra headless script: dump raw bytes at one or more memory ranges.
//
//   -postScript DumpMem.java <outDir> <addr:len> [<addr:len> ...]
//
// Writes mem_<addr>.bin (raw) and mem_<addr>.txt (hex dump) per range.
// Addresses accept the "0x" hex form used by the REA/Ghidra notes.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintWriter;

public class DumpMem extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DumpMem <outDir> <addr:len> [...]");
            return;
        }
        String outDir = args[0];
        new File(outDir).mkdirs();
        Memory mem = currentProgram.getMemory();

        for (int i = 1; i < args.length; i++) {
            String spec = args[i];
            int colon = spec.lastIndexOf(':');
            if (colon < 0) {
                println("bad spec: " + spec);
                continue;
            }
            Address addr = toAddr(spec.substring(0, colon));
            int len = Integer.parseInt(spec.substring(colon + 1).trim());

            byte[] buf = new byte[len];
            mem.getBytes(addr, buf);

            String tag = spec.substring(0, colon).replace("0x", "").replace(":", "_");
            try (FileOutputStream fos = new FileOutputStream(new File(outDir, "mem_" + tag + ".bin"))) {
                fos.write(buf);
            }

            StringBuilder sb = new StringBuilder();
            for (int k = 0; k < buf.length; k++) {
                if (k % 16 == 0) {
                    sb.append(String.format("%n%08x  ", addr.getOffset() + k));
                }
                sb.append(String.format("%02x ", buf[k] & 0xff));
            }
            String text = sb.toString();
            try (PrintWriter pw = new PrintWriter(new File(outDir, "mem_" + tag + ".txt"))) {
                pw.println(text);
            }
            println("=== " + spec + " ===");
            println(text);
        }
    }
}
