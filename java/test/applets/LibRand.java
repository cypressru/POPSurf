import java.applet.Applet;
import java.awt.Graphics;
import java.util.NoSuchElementException;
import java.util.Random;
import java.util.StringTokenizer;

/* java.util.Random against the real JVM, seeded.

   Random is the one class in this set whose algorithm is published in the API
   documentation rather than only in an implementation - a 48-bit linear
   congruential generator with stated constants - so a seeded applet is
   entitled to the same sequence here as anywhere else. That is what this
   compares: if one number differs, the generator is wrong, and an applet that
   places stars or raindrops from a seed would draw a different picture. */
public class LibRand extends Applet {
    boolean done;

    public void paint(Graphics g) {
        if (!done) { done = true; run(); }
    }

    public static void main(String[] a) { run(); }

    static void p(String s) { System.out.println(s); }

    static void run() {
        Random r = new Random(42L);
        StringBuffer b = new StringBuffer("nextInt");
        int i;

        for (i = 0; i < 8; i++) b.append(" ").append(r.nextInt());
        p(b.toString());

        r = new Random(12345L);
        b = new StringBuffer("bounded");
        for (i = 0; i < 12; i++) b.append(" ").append(r.nextInt(100));
        p(b.toString());

        r = new Random(1L);
        b = new StringBuffer("pow2");
        for (i = 0; i < 8; i++) b.append(" ").append(r.nextInt(64));
        p(b.toString());

        r = new Random(7L);
        b = new StringBuffer("double");
        for (i = 0; i < 5; i++) b.append(" ").append(r.nextDouble());
        p(b.toString());

        r = new Random(7L);
        b = new StringBuffer("float");
        for (i = 0; i < 5; i++) b.append(" ").append(r.nextFloat());
        p(b.toString());

        r = new Random(99L);
        b = new StringBuffer("long");
        for (i = 0; i < 4; i++) b.append(" ").append(r.nextLong());
        p(b.toString());

        r = new Random(11L);
        b = new StringBuffer("gauss");
        for (i = 0; i < 5; i++) b.append(" ").append(r.nextGaussian());
        p(b.toString());

        r = new Random(3L);
        b = new StringBuffer("bool");
        for (i = 0; i < 8; i++) b.append(" ").append(r.nextBoolean());
        p(b.toString());

        r = new Random();
        r.setSeed(2024L);
        b = new StringBuffer("setSeed");
        for (i = 0; i < 5; i++) b.append(" ").append(r.nextInt(1000));
        p(b.toString());

        /* An unseeded generator cannot be compared against anything, so only
           its range is checked. */
        r = new Random();
        boolean inRange = true;
        for (i = 0; i < 200; i++) {
            int n = r.nextInt(10);
            if (n < 0 || n > 9) inRange = false;
        }
        p("unseeded-range " + inRange);

        /* --- StringTokenizer --- */
        StringTokenizer t = new StringTokenizer("alpha beta  gamma");
        b = new StringBuffer("tok " + t.countTokens());
        while (t.hasMoreTokens()) b.append(" [").append(t.nextToken()).append("]");
        p(b.toString());

        t = new StringTokenizer("a,b,,c", ",");
        b = new StringBuffer("comma " + t.countTokens());
        while (t.hasMoreTokens()) b.append(" [").append(t.nextToken()).append("]");
        p(b.toString());

        t = new StringTokenizer("  leading and trailing  ");
        b = new StringBuffer("pad " + t.countTokens());
        while (t.hasMoreTokens()) b.append(" [").append(t.nextToken()).append("]");
        p(b.toString());

        t = new StringTokenizer("", ",");
        p("emptytok " + t.countTokens() + " " + t.hasMoreTokens());

        t = new StringTokenizer(",,,", ",");
        p("alldelim " + t.countTokens() + " " + t.hasMoreTokens());

        t = new StringTokenizer("x=1;y=2", "=;");
        b = new StringBuffer("multi " + t.countTokens());
        while (t.hasMoreTokens()) b.append(" [").append(t.nextToken()).append("]");
        p(b.toString());

        t = new StringTokenizer("a b", " ");
        t.nextToken();
        t.nextToken();
        try { t.nextToken(); p("no throw"); }
        catch (NoSuchElementException e) { p("nextToken past end throws"); }

        t = new StringTokenizer("one two three");
        p("countdown " + t.countTokens() + " " + t.nextToken() + " "
          + t.countTokens());
    }
}
