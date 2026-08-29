/* The canonical <param> applet: a marquee whose text, colours, speed and
   position all come from the page and none from the code.

   Shaped like the banner applets that were on every second homepage of the
   period - Netscape's own sample was this, and so was every "cool scrolling
   text" download. It is the case that matters most, because an applet like
   this without getParameter does not fail: it runs, draws its fallback
   string in its fallback colours, and looks like a working applet showing
   the wrong thing.

   Every read below is written the way period code wrote it - fetch, test for
   null, fall back - so a runtime that returns "" instead of null, or that
   matches the name case-sensitively, produces a visibly different picture. */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;

public class Scroller extends Applet implements Runnable {
    String text = "no text parameter";
    Color  fg   = Color.gray;
    Color  bg   = Color.black;
    int    speed = 1;
    int    x, y;
    boolean configured;
    Thread t;

    public void init() {
        String s = getParameter("text");
        if (s != null) { text = s; configured = true; }

        /* Deliberately asked for in a different case than the page writes it.
           A browser folds the case; matching exactly leaves this null and the
           banner draws in its fallback colour. */
        fg = parseColor(getParameter("TEXTCOLOR"), Color.gray);
        bg = parseColor(getParameter("bgcolor"), Color.black);

        String sp = getParameter("Speed");
        if (sp != null) speed = digits(sp);
        if (speed <= 0) speed = 1;

        /* Absent on purpose: nothing in the page names it, so this must come
           back null and leave the default alone. A runtime that hands back an
           empty string instead puts the text at the top of the box. */
        String at = getParameter("ypos");
        y = at != null ? digits(at) : 100;

        x = 300;

        /* The era's own debugging channel, and here the thing that makes the
           test readable without comparing images: a missing parameter and a
           wrongly-cased one look identical in a frame and different here. */
        System.out.println("text=" + text);
        System.out.println("fg=" + fg.getRGB() + " bg=" + bg.getRGB());
        System.out.println("speed=" + speed + " y=" + y);
        System.out.println("codeBase=" + getCodeBase());
        System.out.println("docBase=" + getDocumentBase());
        System.out.println("absent-is-null="
                           + (getParameter("nosuchthing") == null));
        System.out.println("empty-is-null="
                           + (getParameter("empty") == null));
    }

    public void start() { t = new Thread(this); t.start(); }

    public void run() {
        while (true) {
            x -= speed;
            if (x < -240) x = 300;
            repaint();
            try { Thread.sleep(40); } catch (Exception e) { }
        }
    }

    public void paint(Graphics g) {
        g.setColor(bg);
        g.fillRect(0, 0, 300, 200);

        g.setColor(fg);
        g.drawString(text, x, y);

        /* A second line stating what was read, so a golden frame is not the
           only way to tell a wrong value from a missing one. */
        g.setColor(Color.white);
        g.drawString(configured ? "cfg speed=" + speed + " y=" + y
                                : "UNCONFIGURED", 8, 20);
    }

    /* #rrggbb, or one of the three names the test page uses. No
       Integer.parseInt: the runtime does not have it yet. */
    Color parseColor(String s, Color dflt) {
        if (s == null) return dflt;
        if (s.equals("red")) return Color.red;
        if (s.equals("cyan")) return Color.cyan;
        if (s.equals("yellow")) return Color.yellow;
        if (s.length() == 7 && s.charAt(0) == '#')
            return new Color(hex(s, 1), hex(s, 3), hex(s, 5));
        return dflt;
    }

    int hex(String s, int at) {
        return nib(s.charAt(at)) * 16 + nib(s.charAt(at + 1));
    }

    int nib(int c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    }

    int digits(String s) {
        int n = 0;
        for (int i = 0; i < s.length(); i++) {
            int c = s.charAt(i);
            if (c < '0' || c > '9') break;
            n = n * 10 + (c - '0');
        }
        return n;
    }
}
