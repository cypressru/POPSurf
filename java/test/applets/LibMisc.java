import java.applet.Applet;
import java.awt.Graphics;

/* StringBuffer and the two System calls an applet makes for reasons other than
   printing. currentTimeMillis is deliberately not compared against the JVM -
   it cannot be - so only the properties that must hold are checked. */
public class LibMisc extends Applet {
    boolean done;

    public void paint(Graphics g) {
        if (!done) { done = true; run(); }
    }

    public static void main(String[] a) { run(); }

    static void p(String s) { System.out.println(s); }

    static void run() {
        StringBuffer b = new StringBuffer();

        b.append("ab").append(1).append(true).append('c').append(2L);
        p("append [" + b.toString() + "] " + b.length());

        b.append(1.5).append(" ").append(2.5f);
        p("appendfp [" + b.toString() + "]");

        b = new StringBuffer("seed");
        p("ctor [" + b.toString() + "] " + b.length() + " " + b.charAt(0));

        b = new StringBuffer(64);
        b.append("cap");
        p("capctor [" + b.toString() + "] " + b.length());

        b = new StringBuffer("abcdef");
        b.setLength(3);
        p("setLength [" + b.toString() + "] " + b.length());
        b.setLength(5);
        /* The pad is NUL, and a NUL cannot travel through the browser's log -
           printf stops at it - so the characters are reported as numbers. */
        p("grow " + b.length() + " " + (int) b.charAt(2) + " "
          + (int) b.charAt(3) + " " + (int) b.charAt(4));

        b = new StringBuffer("abcdef");
        b.setCharAt(0, 'A');
        p("setCharAt [" + b.toString() + "]");
        p("reverse [" + new StringBuffer("abcdef").reverse().toString() + "]");
        p("insert [" + new StringBuffer("acd").insert(1, "b").toString()
          + "] [" + new StringBuffer("bcd").insert(0, 'a').toString()
          + "] [" + new StringBuffer("ab").insert(2, 12).toString() + "]");

        b = new StringBuffer("abcdef");
        p("delete [" + b.deleteCharAt(2).toString() + "]");

        b = new StringBuffer("chain");
        p("chain [" + b.append("-").append(b.length()).toString() + "]");

        p("bufobj [" + new StringBuffer().append((Object) "obj").toString()
          + "]");

        try { new StringBuffer("abc").charAt(9); p("no throw"); }
        catch (StringIndexOutOfBoundsException e) { p("charAt(9) throws"); }

        /* --- System --- */
        int[] src = { 1, 2, 3, 4, 5 };
        int[] dst = new int[5];

        System.arraycopy(src, 0, dst, 0, 5);
        p("copy " + dst[0] + dst[1] + dst[2] + dst[3] + dst[4]);

        System.arraycopy(src, 1, dst, 0, 3);
        p("copyoff " + dst[0] + dst[1] + dst[2] + dst[3] + dst[4]);

        /* Overlapping, forwards and backwards: the spec says this behaves as
           if through a temporary, so both directions must be intact. */
        int[] ov = { 1, 2, 3, 4, 5 };
        System.arraycopy(ov, 0, ov, 1, 4);
        p("overlap-up " + ov[0] + ov[1] + ov[2] + ov[3] + ov[4]);
        int[] ov2 = { 1, 2, 3, 4, 5 };
        System.arraycopy(ov2, 1, ov2, 0, 4);
        p("overlap-down " + ov2[0] + ov2[1] + ov2[2] + ov2[3] + ov2[4]);

        String[] so = { "a", "b", "c" };
        String[] sd = new String[3];
        System.arraycopy(so, 0, sd, 0, 3);
        p("copyref " + sd[0] + sd[1] + sd[2]);

        try { System.arraycopy(src, 0, dst, 0, 99); p("no throw"); }
        catch (ArrayIndexOutOfBoundsException e) { p("arraycopy(99) throws"); }

        long t0 = System.currentTimeMillis();
        long t1 = System.currentTimeMillis();
        p("clock " + (t0 > 0L) + " " + (t1 >= t0));
    }
}
