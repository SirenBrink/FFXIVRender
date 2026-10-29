import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import java.io.*;
import java.util.*;

public class MenuLoadStateAudit extends GhidraScript {
    private PrintWriter output;
    private DecompInterface decompiler;
    private Set<Long> seen = new HashSet<>();
    private TreeMap<Long, Long> functions = new TreeMap<>();
    private Map<Long, Long> roots = new HashMap<>();
    private long base;
    private void dump(long location) throws Exception {
        Map.Entry<Long, Long> range = functions.floorEntry(location);
        long root = range == null || location >= range.getValue() ? location : roots.getOrDefault(range.getKey(),range.getKey());
        if (!seen.add(root)) return;
        Address start = toAddr(root);
        disassemble(start);
        Function fn = getFunctionAt(start);
        if (fn == null) fn = createFunction(start, null);
        if (fn == null) { output.println("Failed creating function " + start); return; }
        output.println("\nFUNCTION " + fn.getName() + " @ " + start + " ref " + Long.toHexString(location));
        byte[] bytes = new byte[32]; currentProgram.getMemory().getBytes(start, bytes);
        output.print("ENTRY BYTES:"); for (byte b:bytes) output.printf(" %02X", b & 255); output.println();
        DecompileResults result = decompiler.decompileFunction(fn, 60, monitor);
        if (result.decompileCompleted()) output.println(result.getDecompiledFunction().getC());
        else output.println(result.getErrorMessage());
        output.flush();
    }
    public void run() throws Exception {
        output = new PrintWriter("C:/Games/FF14/XIVRender/diagnostics/menu-load-state-20261024.txt");
        output.println("Executable MD5: " + currentProgram.getExecutableMD5());
        base = currentProgram.getImageBase().getOffset();
        Memory memory = currentProgram.getMemory();
        MemoryBlock pdata = memory.getBlock(".pdata");
        for (long pos = 0; pos + 12 <= pdata.getSize(); pos += 12) {
            Address a = pdata.getStart().add(pos);
            long begin = Integer.toUnsignedLong(memory.getInt(a));
            long end = Integer.toUnsignedLong(memory.getInt(a.add(4)));
            if (begin != 0 && end > begin) {
                functions.put(base+begin, base+end);
                long root=begin;
                long unwind=Integer.toUnsignedLong(memory.getInt(a.add(8)));
                for (int n=0;n<10;n++) {
                    Address ua=toAddr(base+unwind);
                    if ((memory.getByte(ua)&0x20)==0) break;
                    int count=Byte.toUnsignedInt(memory.getByte(ua.add(2)));
                    Address chained=ua.add(4+((count+1)&~1)*2);
                    root=Integer.toUnsignedLong(memory.getInt(chained));
                    unwind=Integer.toUnsignedLong(memory.getInt(chained.add(8)));
                }
                roots.put(base+begin,base+root);
            }
        }
        decompiler = new DecompInterface(); decompiler.openProgram(currentProgram);
        dump(base+0x4CDB90); dump(base+0x8CE880); dump(base+0x602720); dump(base+0x602B80); dump(base+0x602F80); dump(base+0x603E20);
        output.close(); decompiler.dispose();
    }
    private void probe(String name,String sig,boolean call) throws Exception {
        String[] words=sig.split(" ");byte[] pattern=new byte[words.length],mask=new byte[words.length];
        for(int i=0;i<words.length;i++)if(!words[i].equals("??")){pattern[i]=(byte)Integer.parseInt(words[i],16);mask[i]=(byte)255;}
        Address from=currentProgram.getMemory().getBlock(".text").getStart();
        Address end=currentProgram.getMemory().getBlock(".text").getEnd();
        for(int n=0;n<8;n++){
            Address match=currentProgram.getMemory().findBytes(from,end,pattern,mask,true,monitor);if(match==null)break;
            output.println("SIGNATURE "+name+" "+match);
            if(call){long target=match.getOffset()+5+currentProgram.getMemory().getInt(match.add(1));dump(target);dump(match.getOffset());}
            else output.println("GLOBAL "+Long.toHexString(match.getOffset()+7+currentProgram.getMemory().getInt(match.add(3))));
            output.flush();from=match.add(1);
        }
    }
}
