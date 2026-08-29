/* Two things that used to be quietly wrong.
 *
 * Both are the bad kind of bug: the applet runs, exits green, and draws or
 * prints something that is not what the author wrote. Neither would show up in
 * a frame comparison against ourselves, only against a real JVM - which is
 * what this file is for. Every line below is diffed against `java Fixes`.
 */
import java.applet.Applet;
import java.awt.Graphics;

public class Fixes extends Applet {

    /* An object with a toString. String concatenation compiles to
     * StringBuilder.append(Object), which used to print the address. */
    static class Pt {
        int x, y;
        Pt(int x, int y) { this.x = x; this.y = y; }
        public String toString() { return "Pt[" + x + "," + y + "]"; }
    }

    /* One without, to prove the fallback still happens rather than the call
     * being made on anything that happens to be an object. */
    static class Bare {
        int v = 3;
    }

    static String check() {
        StringBuffer sb = new StringBuffer();

        sb.append("concat=" + new Pt(3, 4) + "\n");
        sb.append("nested=" + new Pt(1, 2) + "/" + new Pt(5, 6) + "\n");
        sb.append("nullobj=" + (Object)null + "\n");
        sb.append("bare=" + (new Bare().toString().startsWith("Fixes$Bare@")
                             ? "default" : "unexpected") + "\n");

        /* A subclass caught by its superclass. String.substring throws
         * StringIndexOutOfBounds; an author validating an index writes the
         * general one, and matching on the exact name missed it - so a guard
         * written correctly did not fire. */
        try {
            "abc".substring(9);
            sb.append("sub=nothrow\n");
        } catch(IndexOutOfBoundsException e) {
            sb.append("sub=caught-as-IndexOutOfBounds\n");
        }

        try {
            int[] a = new int[2];
            a[5] = 1;
            sb.append("arr=nothrow\n");
        } catch(IndexOutOfBoundsException e) {
            sb.append("arr=caught-as-IndexOutOfBounds\n");
        }

        /* NumberFormatException is an IllegalArgumentException, which is the
         * other one applets lean on when they validate a <param>. */
        try {
            Integer.parseInt("not a number");
            sb.append("num=nothrow\n");
        } catch(IllegalArgumentException e) {
            sb.append("num=caught-as-IllegalArgument\n");
        }

        /* And the exact type still works, so the chain did not replace the
         * old rule with a looser one. */
        try {
            "abc".substring(9);
        } catch(StringIndexOutOfBoundsException e) {
            sb.append("exact=caught\n");
        }

        /* A catch that must NOT fire: ArithmeticException is not on the path
         * from StringIndexOutOfBounds to Throwable. */
        String outcome = "escaped";
        try {
            try {
                "abc".substring(9);
            } catch(ArithmeticException e) {
                outcome = "wrongly-caught";
            }
        } catch(IndexOutOfBoundsException e) {
            outcome = "escaped-to-outer";
        }
        sb.append("unrelated=" + outcome + "\n");

        return sb.toString();
    }

    public static void main(String[] args) {
        System.out.print(check());
    }

    public void init() {
        System.out.print(check());
    }

    public void paint(Graphics g) {
        g.drawString("fixes", 10, 20);
    }
}
