/* getAudioClip and the AudioClip interface, in the shape period applets used
   it: load the clips in init(), keep them in fields, start a background loop
   in start() and fire a one-shot from a mouse click.

   Before AudioClip existed in this runtime, the getAudioClip call alone
   stopped the interpreter dead and the box stayed blank - so what this proves
   first is that an applet which loads sound still draws. It also checks the
   two-argument form against getCodeBase() and the one-argument form against a
   URL that has already been resolved, since those are the two ways the era
   spelled it.

   There is no sound. See the note in ps_applet.c. */
import java.applet.Applet;
import java.applet.AudioClip;
import java.awt.Color;
import java.awt.Graphics;

public class Chime extends Applet {
    AudioClip ding;
    AudioClip music;
    AudioClip missing;
    int hits;
    boolean looping;

    public void init() {
        String name = getParameter("sound");
        if (name == null) name = "ding.au";

        ding  = getAudioClip(getCodeBase(), name);
        music = getAudioClip(getDocumentBase(), "loop.au");

        /* A clip the page never provides. A real AWT still hands back an
           object here and only plays nothing, so this must not be null and
           must not throw when it is played. */
        missing = getAudioClip(getCodeBase(), "nothing-here.au");

        System.out.println("clips=" + count());
        System.out.println("missing-is-null=" + (missing == null));
    }

    public void start() {
        if (music != null) { music.loop(); looping = true; }
        System.out.println("looping=" + looping);
    }

    public void stop() {
        if (music != null) music.stop();
        looping = false;
    }

    public boolean mouseDown(java.awt.Event e, int x, int y) {
        if (ding != null) ding.play();
        if (missing != null) missing.play();
        hits++;
        repaint();
        return true;
    }

    public void paint(Graphics g) {
        g.setColor(new Color(16, 18, 28));
        g.fillRect(0, 0, 300, 200);

        g.setColor(Color.white);
        g.drawString("clips " + count() + "  plays " + hits, 12, 24);
        g.setColor(looping ? Color.green : Color.red);
        g.drawString(looping ? "looping" : "silent", 12, 48);

        g.setColor(Color.cyan);
        g.drawString("codeBase " + getCodeBase(), 12, 76);
        g.drawString("docBase  " + getDocumentBase(), 12, 100);

        g.setColor(Color.yellow);
        g.fillOval(120, 120, 60, 60);
    }

    int count() {
        int n = 0;
        if (ding != null) n++;
        if (music != null) n++;
        if (missing != null) n++;
        return n;
    }
}
