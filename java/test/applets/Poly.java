/* java.awt.Polygon, java.util.Stack and Math.log, against the real JDK.
 *
 * Everything here prints rather than draws, so `java Poly` and the host
 * harness produce the same transcript and the two can be diffed exactly. That
 * comparison is the point: the interesting parts of Polygon are the edges -
 * whether the constructor copies its arrays, how long they end up, which side
 * of an edge contains() calls inside, and the fact that getBounds caches and
 * goes visibly stale - and every one of them was settled by asking a real
 * implementation rather than by reasoning.
 *
 * Two things are deliberately not printed. Doubles, because this runtime's
 * StringBuffer formats them with %g and a real one does not, so Math.log is
 * checked as a scaled integer. And Polygon.toString, because the real one
 * inherits Object's and prints an address.
 */
import java.applet.Applet;
import java.awt.Graphics;
import java.awt.Point;
import java.awt.Polygon;
import java.awt.Rectangle;
import java.util.EmptyStackException;
import java.util.Stack;

public class Poly extends Applet {
    boolean done;

    public void paint(Graphics g) {
        if (!done) { done = true; run(); }
    }

    public static void main(String[] a) { run(); }

    static void p(String s) { System.out.println(s); }

    /* "x" + rect compiles to StringBuffer.append(Object), and this runtime's
       StringBuffer does not call toString on what it is handed. Calling it
       here keeps this a test of the geometry rather than of that. */
    static String s(Rectangle r) { return r.toString(); }

    static String pts(Polygon q) {
        StringBuffer b = new StringBuffer("[");
        for (int i = 0; i < q.npoints; i++) {
            if (i > 0) b.append(" ");
            b.append(q.xpoints[i]);
            b.append(",");
            b.append(q.ypoints[i]);
        }
        b.append("]");
        return b.toString();
    }

    static Polygon square() {
        return new Polygon(new int[] { 0, 10, 10, 0 },
                           new int[] { 0, 0, 10, 10 }, 4);
    }

    static void shape(String tag, Polygon q, int[] xs, int[] ys) {
        StringBuffer b = new StringBuffer();
        for (int i = 0; i < xs.length; i++) {
            b.append(" ");
            b.append(xs[i]);
            b.append(",");
            b.append(ys[i]);
            b.append("=");
            b.append(q.contains(xs[i], ys[i]));
        }
        p(tag + b.toString());
    }

    static void run() {
        polygons();
        stacks();
        maths();
    }

    static void polygons() {
        Polygon e = new Polygon();
        p("empty n " + e.npoints + " len " + e.xpoints.length + " "
          + e.ypoints.length);
        p("empty bounds " + s(e.getBounds()) + " contains " + e.contains(0, 0));

        /* The arrays are copied, not aliased, and exactly npoints long. Both
           halves are observable and applets rely on the first: they build a
           scratch array once and hand it to several polygons. */
        int[] xs = { 0, 10, 10, 0 };
        int[] ys = { 0, 0, 10, 10 };
        Polygon q = new Polygon(xs, ys, 4);
        xs[0] = 99;
        p("copy n " + q.npoints + " len " + q.xpoints.length + " x0 "
          + q.xpoints[0] + " bounds " + s(q.getBounds()));

        Polygon r = new Polygon(new int[] { 1, 2, 3, 4, 5 },
                                new int[] { 1, 2, 3, 4, 5 }, 3);
        p("short n " + r.npoints + " len " + r.xpoints.length + " bounds "
          + s(r.getBounds()));

        Polygon z0 = new Polygon(new int[0], new int[0], 0);
        p("zero n " + z0.npoints + " len " + z0.xpoints.length + " bounds "
          + s(z0.getBounds()));

        /* Growth: the next power of two strictly greater than the count, with
           a floor of four. */
        Polygon g = new Polygon();
        StringBuffer caps = new StringBuffer();
        for (int i = 0; i < 10; i++) {
            g.addPoint(i, i * 2);
            caps.append(" ");
            caps.append(g.npoints);
            caps.append("/");
            caps.append(g.xpoints.length);
        }
        p("grow" + caps.toString());
        p("grown pts " + pts(g) + " bounds " + s(g.getBounds()));

        for (int i = 1; i <= 9; i++) {
            Polygon k = new Polygon(new int[i], new int[i], i);
            k.addPoint(1, 1);
            p("capfrom " + i + " -> " + k.xpoints.length);
        }

        /* The bounds cache, and the reason invalidate() is public API. An
           applet that writes into xpoints itself gets the old box back. */
        Polygon m = square();
        p("cache1 " + s(m.getBounds()));
        m.xpoints[0] = -50;
        p("cache2 " + s(m.getBounds()));
        m.invalidate();
        p("cache3 " + s(m.getBounds()));

        Polygon st = square();
        p("stale contains before " + st.contains(15, 5));
        st.xpoints[1] = 20;
        st.xpoints[2] = 20;
        p("stale contains after " + st.contains(15, 5));
        st.invalidate();
        p("stale contains fresh " + st.contains(15, 5));

        Polygon nf = square();
        nf.xpoints[1] = 20;
        nf.xpoints[2] = 20;
        p("never-asked contains " + nf.contains(15, 5));

        Polygon t = square();
        t.getBounds();
        t.translate(3, -4);
        p("translate " + pts(t) + " " + s(t.getBounds()));

        Polygon ad = square();
        ad.getBounds();
        ad.addPoint(40, -20);
        p("addPoint bounds " + s(ad.getBounds()));

        Polygon rs = square();
        rs.getBounds();
        rs.reset();
        p("reset n " + rs.npoints + " len " + rs.xpoints.length + " bounds "
          + s(rs.getBounds()));
        rs.addPoint(7, 8);
        p("reset reuse " + pts(rs) + " len " + rs.xpoints.length + " bounds "
          + s(rs.getBounds()));

        /* npoints written down by hand, which applets do to reuse a polygon
           without reallocating it. */
        Polygon w = square();
        w.npoints = 3;
        p("hand n " + w.npoints + " bounds " + s(w.getBounds())
          + " contains 9,9 " + w.contains(9, 9));

        /* contains: inside, outside, on an edge, on a vertex. The left and
           top edges are in and the right and bottom edges are out, which is
           the half nobody guesses. */
        Polygon sq = square();
        shape("sq", sq, new int[] { 5, 0, 10, 0, 10, 5, 5, -1, 11, 9, 10, 0 },
                        new int[] { 5, 0, 10, 5, 5, 0, 10, 5, 5, 9, 0, 10 });
        p("sq inside " + sq.inside(5, 5) + " " + sq.inside(10, 10));
        p("sq point " + sq.contains(new Point(5, 5)) + " "
          + sq.contains(new Point(50, 50)));
        p("sq bbox " + s(sq.getBoundingBox()));

        /* A concave notch: the C-shape's bay is outside even though it is
           inside the bounding box. */
        Polygon c = new Polygon(new int[] { 0, 30, 30, 10, 10, 30, 30, 0 },
                                new int[] { 0, 0, 10, 10, 20, 20, 30, 30 }, 8);
        shape("notch", c, new int[] { 5, 20, 20, 20, 15, 29, 31 },
                          new int[] { 15, 15, 5, 25, 15, 15, 15 });

        /* Self-intersecting, which is the only shape where even-odd and
           nonzero winding disagree: the middle of a five-pointed star is
           hollow under even-odd, and java.awt.Polygon is even-odd. */
        Polygon star = new Polygon();
        star.addPoint(50, 10);
        star.addPoint(26, 82);
        star.addPoint(88, 38);
        star.addPoint(12, 38);
        star.addPoint(74, 82);
        p("star " + pts(star) + " " + s(star.getBounds()));
        shape("star", star, new int[] { 50, 50, 50, 20, 50, 50, 0 },
                            new int[] { 50, 20, 15, 50, 80, 45, 0 });

        /* A diagonal edge, where the crossing lands between two pixels. */
        Polygon tri = new Polygon(new int[] { 0, 20, 0 },
                                  new int[] { 0, 0, 20 }, 3);
        shape("tri", tri, new int[] { 1, 5, 10, 11, 19, 0, 6 },
                          new int[] { 1, 5, 10, 11, 0, 19, 3 });

        Polygon neg = new Polygon(new int[] { -10, -2, -6 },
                                  new int[] { -10, -10, -2 }, 3);
        p("neg " + s(neg.getBounds()) + " " + neg.contains(-6, -8));

        /* One and two points: too few to enclose anything, whatever the
           bounding box says. */
        Polygon d = new Polygon();
        d.addPoint(5, 5);
        p("one " + s(d.getBounds()) + " " + d.contains(5, 5));
        d.addPoint(15, 25);
        p("two " + s(d.getBounds()) + " " + d.contains(10, 15));

        /* What the constructor refuses. */
        try {
            new Polygon(new int[2], new int[2], 3);
            p("ctor short: no throw");
        } catch (IndexOutOfBoundsException ex) {
            p("ctor short: IndexOutOfBounds");
        }
        try {
            new Polygon(new int[2], new int[2], -1);
            p("ctor neg: no throw");
        } catch (NegativeArraySizeException ex) {
            p("ctor neg: NegativeArraySize");
        }
        try {
            new Polygon(null, null, 0);
            p("ctor null: no throw");
        } catch (NullPointerException ex) {
            p("ctor null: NullPointer");
        }
    }

    static void stacks() {
        Stack s = new Stack();
        p("st empty " + s.empty() + " size " + s.size());

        p("st push " + (String) s.push("a"));
        s.push("b");
        s.push("c");
        p("st after " + s.size() + " " + s.empty() + " peek "
          + (String) s.peek());

        /* search is one-based and counted down from the top, which is the
           part that is never what you expect. */
        p("st search " + s.search("c") + " " + s.search("b") + " "
          + s.search("a") + " " + s.search("z"));

        p("st pop " + (String) s.pop() + " " + (String) s.pop() + " size "
          + s.size());

        /* A Stack is a Vector, so the Vector half has to keep working on the
           same object and see the same elements. */
        s.push("d");
        p("st vec " + s.size() + " " + (String) s.elementAt(0) + " "
          + (String) s.elementAt(1) + " " + s.contains("d") + " "
          + s.indexOf("d") + " " + s.isEmpty());
        s.insertElementAt("z", 0);
        p("st insert " + s.size() + " top " + (String) s.peek() + " search "
          + s.search("z"));
        s.removeAllElements();
        p("st cleared " + s.size() + " " + s.empty());

        try {
            s.pop();
            p("st pop empty: no throw");
        } catch (EmptyStackException ex) {
            p("st pop empty: EmptyStack");
        }
        try {
            s.peek();
            p("st peek empty: no throw");
        } catch (EmptyStackException ex) {
            p("st peek empty: EmptyStack");
        }

        /* Duplicates: search reports the one nearest the top. */
        Stack dup = new Stack();
        dup.push("x");
        dup.push("y");
        dup.push("x");
        p("st dup " + dup.search("x") + " " + dup.search("y"));

        /* Enough traffic to force a collection with the elements reachable
           only from the stack, which is the thing that would fail if the
           storage were not an ordinary traced array. */
        Stack big = new Stack();
        for (int i = 0; i < 3000; i++)
            big.push("item " + i);
        int bad = 0;
        for (int i = 2999; i >= 0; i--)
            if (!((String) big.pop()).equals("item " + i))
                bad++;
        p("st churn " + bad + " " + big.size() + " " + big.empty());
    }

    static void maths() {
        /* Scaled to an int: a double printed by this runtime's StringBuffer
           and a double printed by a real one do not agree on format. */
        p("log1 " + (int) (Math.log(1.0) * 1000000.0));
        p("logE " + (int) (Math.log(Math.E) * 1000000.0));
        p("log2 " + (int) (Math.log(2.0) * 1000000.0));
        p("log10 " + (int) (Math.log(10.0) * 1000000.0));
        p("loghalf " + (int) (Math.log(0.5) * 1000000.0));
        p("logbig " + (int) (Math.log(1.0e9) * 1000.0));
        p("log0 " + (Math.log(0.0) < -1.0e300));
        /* log(-1) is NaN and is not checked here, because this runtime's
           dcmpl/dcmpg push 0 for an unordered comparison where the JVM spec
           says -1 and 1 - so `x != x` is false for every NaN, not only this
           one. That is a bug in the interpreter and not in Math.log, and it
           belongs with whoever owns ps_jvm.c. */
    }
}
