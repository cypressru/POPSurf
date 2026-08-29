import java.applet.Applet;
import java.awt.Graphics;

/* java.lang.String, against the real JVM.

   Runs the same body two ways: `java LibString` on a desktop JDK prints the
   expected output, and the browser's runtime prints it again through
   System.out when the applet is painted. The two are diffed, which is the only
   way to settle the edge cases - what substring does at the ends, which
   indexOf overload counts from where - without a spec to hand. */
public class LibString extends Applet {
    boolean done;

    public void paint(Graphics g) {
        if (!done) { done = true; run(); }
    }

    public static void main(String[] a) { run(); }

    static void p(String s) { System.out.println(s); }

    static void run() {
        String s = "Hello, World";

        p("length " + s.length());
        p("charAt " + s.charAt(0) + s.charAt(4) + s.charAt(11));

        p("sub1 [" + s.substring(7) + "]");
        p("sub2 [" + s.substring(0, 5) + "]");
        p("sub-empty [" + s.substring(5, 5) + "]");
        p("sub-all [" + s.substring(0, s.length()) + "]");
        p("sub-end [" + s.substring(s.length()) + "]");

        try { p("bad " + s.substring(-1)); }
        catch (StringIndexOutOfBoundsException e) { p("sub(-1) throws"); }
        try { p("bad " + s.substring(99)); }
        catch (StringIndexOutOfBoundsException e) { p("sub(99) throws"); }
        try { p("bad " + s.substring(4, 2)); }
        catch (StringIndexOutOfBoundsException e) { p("sub(4,2) throws"); }
        try { p("bad " + s.substring(0, 99)); }
        catch (StringIndexOutOfBoundsException e) { p("sub(0,99) throws"); }
        try { p("bad " + s.charAt(99)); }
        catch (StringIndexOutOfBoundsException e) { p("charAt(99) throws"); }

        p("idxc " + s.indexOf('o') + " " + s.indexOf('o', 5) + " "
          + s.indexOf('z'));
        p("lidxc " + s.lastIndexOf('o') + " " + s.lastIndexOf('o', 6) + " "
          + s.lastIndexOf('z'));
        p("idxs " + s.indexOf("World") + " " + s.indexOf("o, ") + " "
          + s.indexOf("nope") + " " + s.indexOf(""));
        p("idxs2 " + s.indexOf("o", 5) + " " + s.indexOf("", 4) + " "
          + s.indexOf("", 99));
        p("lidxs " + s.lastIndexOf("o") + " " + s.lastIndexOf("o", 3) + " "
          + s.lastIndexOf("l") + " " + s.lastIndexOf(""));

        p("eq " + s.equals("Hello, World") + s.equals("hello, world")
          + s.equals(null) + "");
        p("eqic " + s.equalsIgnoreCase("HELLO, WORLD")
          + s.equalsIgnoreCase("nope"));
        p("cmp " + s.compareTo("Hello, World") + " " + s.compareTo("Hello")
          + " " + s.compareTo("Z") + " " + "a".compareTo("b"));
        p("cmpic " + s.compareToIgnoreCase("HELLO, WORLD"));

        p("trim [" + "  pad me \t".trim() + "] [" + "".trim() + "] ["
          + "   ".trim() + "]");
        p("case " + s.toUpperCase() + " " + s.toLowerCase());
        p("starts " + s.startsWith("Hello") + s.startsWith("World")
          + s.startsWith("World", 7) + s.startsWith(""));
        p("ends " + s.endsWith("World") + s.endsWith("Hello")
          + s.endsWith(""));
        p("replace [" + s.replace('l', 'L') + "] [" + s.replace('z', 'Z')
          + "]");
        p("concat [" + s.concat("!!") + "] [" + "".concat(s) + "]");

        char[] c = s.toCharArray();
        p("chars " + c.length + " " + c[0] + c[c.length - 1]);
        p("fromchars [" + new String(c) + "] [" + new String(c, 7, 5) + "]");
        p("valueOfChars [" + String.valueOf(c) + "]");

        p("hash " + s.hashCode() + " " + "".hashCode() + " " + "a".hashCode());

        p("vi " + String.valueOf(42) + " " + String.valueOf(-42) + " "
          + String.valueOf(Integer.MIN_VALUE));
        p("vj " + String.valueOf(123456789012345L) + " "
          + String.valueOf(Long.MIN_VALUE));
        p("vz " + String.valueOf(true) + " " + String.valueOf(false));
        p("vc " + String.valueOf('q'));
        p("vo " + String.valueOf((Object) "obj"));

        p("d " + String.valueOf(1.0) + " " + String.valueOf(0.5) + " "
          + String.valueOf(-0.0) + " " + String.valueOf(100.0));
        p("d " + String.valueOf(1.0 / 3.0) + " " + String.valueOf(1.0e7)
          + " " + String.valueOf(9999999.0));
        p("d " + String.valueOf(1.0e-3) + " " + String.valueOf(1.0e-4)
          + " " + String.valueOf(1.0e20));
        p("d " + String.valueOf(1.0 / 0.0) + " " + String.valueOf(-1.0 / 0.0)
          + " " + String.valueOf(0.0 / 0.0));
        p("f " + String.valueOf(1.0f) + " " + String.valueOf(0.5f) + " "
          + String.valueOf(1.0f / 3.0f) + " " + String.valueOf(1.0e10f));

        p("plus " + 1.5 + " " + 2.0f + " " + 'x' + " " + 7L + " " + true);
        p("intern " + ("Hel" + "lo").intern().equals("Hello"));
        p("empty " + "".length() + " " + "".isEmpty() + " " + s.isEmpty());
    }
}
