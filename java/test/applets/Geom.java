/* java.awt geometry, against the real JDK.
 *
 * geometry() prints only things that do not depend on the applet's size or on
 * a font, so it produces the same lines under `java Geom` as it does under the
 * host harness - which is how ps_jgeom.c was checked. Everything the harness
 * prints on top of that is a self-consistency check against the browser's own
 * text backend, which the real JDK has no way to agree with.
 *
 * The s() overloads are not decoration: `"x" + rect` compiles to
 * StringBuilder.append(Object), and this runtime's StringBuilder does not call
 * toString on what it is handed. Calling it here keeps this a test of the
 * geometry rather than of that.
 */
import java.applet.Applet;
import java.awt.Dimension;
import java.awt.Font;
import java.awt.FontMetrics;
import java.awt.Graphics;
import java.awt.Insets;
import java.awt.Point;
import java.awt.Rectangle;

public class Geom extends Applet {

    static void p(String s) { System.out.println(s); }

    static Rectangle R(int x, int y, int w, int h) {
        return new Rectangle(x, y, w, h);
    }

    static String s(Dimension d) { return d.toString(); }
    static String s(Point d)     { return d.toString(); }
    static String s(Rectangle d) { return d.toString(); }
    static String s(Insets d)    { return d.toString(); }

    public static void main(String[] a) { geometry(); }

    Dimension atInit;
    int       damaged;

    /* init() is where a real applet asks its size, several thousand
     * instructions before paint() exists, so the accessors have to answer
     * there too. The loop is allocation pressure: a geometry object is an
     * ordinary heap object and has to survive a collection while a local still
     * refers to it. */
    public void init() {
        atInit = getSize();

        Rectangle keep = new Rectangle(1, 2, 3, 4);
        Point     also = new Point(9, 9);

        for (int i = 0; i < 2000; i++) {
            Dimension t = new Dimension(i, i + 1);
            if (t.width != i || t.height != i + 1)
                damaged++;
        }
        if (keep.x != 1 || keep.y != 2 || keep.width != 3 || keep.height != 4)
            damaged += 100;
        if (also.x != 9 || also.y != 9)
            damaged += 100;
    }

    static void geometry() {
        Dimension d0 = new Dimension();
        Dimension d1 = new Dimension(30, 40);
        Dimension d2 = new Dimension(d1);
        p("d0 " + d0.width + " " + d0.height + " " + s(d0));
        p("d1 " + d1.width + " " + d1.height + " " + s(d1));
        p("d2 " + s(d2) + " eq " + d1.equals(d2) + " " + d1.equals(d0));
        d2.setSize(5, 6);
        p("d2set " + s(d2) + " getSize " + s(d2.getSize()));
        d2.width = 11;
        d2.height = 12;
        p("d2fld " + d2.width + "x" + d2.height);

        Point p0 = new Point();
        Point p1 = new Point(3, 4);
        Point p2 = new Point(p1);
        p("p0 " + s(p0) + " p1 " + s(p1) + " p2 " + s(p2));
        p1.move(7, 8);
        p("p1move " + s(p1));
        p1.translate(2, -3);
        p("p1tr " + s(p1) + " eq " + p1.equals(p2) + " "
          + p2.equals(new Point(3, 4)));
        p1.x = 1;
        p1.y = 2;
        p("p1fld " + p1.x + "," + p1.y + " getLocation " + s(p1.getLocation()));

        Rectangle r0 = new Rectangle();
        Rectangle r1 = R(10, 20, 30, 40);
        Rectangle r2 = new Rectangle(r1);
        Rectangle r3 = new Rectangle(new Dimension(7, 8));
        Rectangle r4 = new Rectangle(5, 6);
        Rectangle r5 = new Rectangle(new Point(1, 2), new Dimension(3, 4));
        p("r0 " + s(r0));
        p("r1 " + s(r1) + " " + r1.x + " " + r1.y + " " + r1.width + " "
          + r1.height);
        p("r2 " + s(r2) + " r3 " + s(r3) + " r4 " + s(r4) + " r5 " + s(r5));
        p("r1 empty " + r1.isEmpty() + " r0 empty " + r0.isEmpty());
        p("contains 10,20 " + r1.contains(10, 20));
        p("contains 39,59 " + r1.contains(39, 59));
        p("contains 40,60 " + r1.contains(40, 60));
        p("contains 9,20 " + r1.contains(9, 20));
        p("contains pt " + r1.contains(new Point(20, 30)));
        p("contains rect " + r1.contains(R(12, 22, 4, 4)));

        Rectangle a1 = R(0, 0, 10, 10);
        Rectangle b1 = R(5, 5, 10, 10);
        Rectangle c1 = R(20, 20, 5, 5);
        Rectangle e1 = R(10, 0, 5, 5);
        p("int ab " + a1.intersects(b1) + " ac " + a1.intersects(c1) + " ae "
          + a1.intersects(e1));
        p("isect ab " + s(a1.intersection(b1)));
        p("isect ac " + s(a1.intersection(c1)));
        p("union ab " + s(a1.union(b1)));
        p("union ac " + s(a1.union(c1)));

        /* The empty-box rules, which are the part nothing else would catch:
         * a negative extent is ignored by union, a zero one is not. */
        Rectangle far = R(100, 100, 0, 0);
        Rectangle neg = R(100, 100, -5, -5);
        Rectangle zh  = R(100, 100, 5, 0);
        p("u zero-far " + s(far.union(a1)) + " " + s(a1.union(far)));
        p("u neg " + s(neg.union(a1)) + " " + s(a1.union(neg)));
        p("u zh " + s(zh.union(a1)) + " " + s(a1.union(zh)));
        p("i box-zero " + s(a1.intersection(far)));
        p("empty " + far.isEmpty() + " " + neg.isEmpty() + " " + zh.isEmpty());
        p("x zero " + far.intersects(a1) + " " + a1.intersects(far));
        p("c zero " + far.contains(100, 100) + " " + a1.contains(R(0, 0, 0, 0))
          + " " + a1.contains(R(5, 5, 10, 10)));

        Rectangle g1 = R(10, 10, 5, 5);
        g1.add(20, 30);
        p("addpt " + s(g1));
        g1.add(new Point(0, 0));
        p("addpt2 " + s(g1));
        g1.add(R(40, 40, 2, 2));
        p("addrect " + s(g1));

        Rectangle e2 = R(100, 100, -1, -1);
        e2.add(0, 0);
        p("add neg " + s(e2));
        Rectangle e3 = R(100, 100, 0, 0);
        e3.add(a1);
        p("addr zero " + s(e3));

        Rectangle s1 = R(1, 2, 3, 4);
        s1.setBounds(9, 8, 7, 6);
        p("setBounds " + s(s1) + " getBounds " + s(s1.getBounds()));
        s1.translate(1, 1);
        p("translate " + s(s1));
        s1.grow(2, 3);
        p("grow " + s(s1));
        s1.setSize(4, 4);
        p("setSize " + s(s1) + " getSize " + s(s1.getSize()));
        s1.setLocation(0, 0);
        p("setLocation " + s(s1) + " getLocation " + s(s1.getLocation()));
        p("req " + r1.equals(R(10, 20, 30, 40)) + " " + r1.equals(R(1, 1, 1, 1)));

        /* Type-checked equals: a Dimension is never equal to a Point, however
         * equal the numbers are. */
        p("eq types " + d1.equals("x") + " " + p1.equals(R(1, 2, 0, 0)) + " "
          + r1.equals(new Dimension(30, 40)));

        Insets in = new Insets(1, 2, 3, 4);
        p("insets " + in.top + " " + in.left + " " + in.bottom + " " + in.right
          + " " + s(in));
    }

    /* The half that only the browser can answer. Printed as relations rather
     * than numbers, so it is a test and not a transcript of one font. */
    void box(Graphics g) {
        Dimension sz = getSize();
        Dimension o  = size();
        Rectangle b  = getBounds();
        Rectangle c  = bounds();

        p("box " + (sz.width == 300 && sz.height == 200) + " "
          + sz.equals(o) + " " + b.equals(c) + " "
          + (b.x == 0 && b.y == 0 && b.width == sz.width
             && b.height == sz.height));
        p("pref " + getPreferredSize().equals(sz) + " loc "
          + s(getLocation()));
        p("init " + atInit.equals(sz) + " survived " + damaged
          + " clip " + s(g.getClipBounds()) + " insets " + s(getInsets()));

        setFont(new Font("Helvetica", Font.PLAIN, 24));
        FontMetrics f24 = getFontMetrics(getFont());
        FontMetrics g24 = g.getFontMetrics();
        p("fm same " + (f24.stringWidth("Hello") == g24.stringWidth("Hello")));

        setFont(new Font("Helvetica", Font.PLAIN, 12));
        FontMetrics f12 = getFontMetrics(getFont());

        int w12 = f12.stringWidth("Centre me");
        int w24 = f24.stringWidth("Centre me");
        p("width>0 " + (w12 > 0) + " scales " + (w24 > w12));
        p("empty " + f12.stringWidth("") + " sums "
          + (f12.stringWidth("ab") == f12.charWidth('a') + f12.charWidth('b')));
        p("vert " + (f12.getAscent() > 0) + " " + (f12.getDescent() > 0) + " "
          + (f12.getHeight()
             == f12.getAscent() + f12.getDescent() + f12.getLeading())
          + " " + (f24.getAscent() > f12.getAscent()));
        p("max " + (f12.getMaxAscent() == f12.getAscent()) + " "
          + (f12.getMaxDescent() == f12.getDescent()));
        p("font " + f24.getFont().getSize() + " " + f12.getFont().getSize());

        /* The reason all of this exists: centring a label. */
        int x = (sz.width - w12) / 2;
        p("centred " + (x > 0 && x + w12 <= sz.width));
    }

    public void paint(Graphics g) {
        geometry();
        box(g);
        g.drawString("Geom", 10, 20);
    }
}
