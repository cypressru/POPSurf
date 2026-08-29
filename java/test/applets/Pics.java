import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.Image;

/* getImage + drawImage, both the plain and the scaled form, plus an image
   that does not exist so the "still in flight / never arrives" path shows. */
public class Pics extends Applet {
    Image a, b, missing;

    public void init() {
        a = getImage(getCodeBase(), "t_pot_a.png");
        b = getImage(getCodeBase(), "t_tile.png");
        missing = getImage(getCodeBase(), "nope.png");
    }

    public void paint(Graphics g) {
        g.setColor(new Color(24, 26, 34));
        g.fillRect(0, 0, 300, 200);

        g.setColor(Color.white);
        g.drawString("drawImage", 12, 20);

        g.drawImage(a, 20, 36, this);
        g.drawImage(b, 90, 36, this);

        g.drawImage(a, 20, 100, 64, 64, this);
        g.drawImage(b, 100, 100, 96, 48, this);

        g.setColor(new Color(200, 60, 50));
        g.drawRect(210, 100, 63, 63);
        g.drawImage(missing, 211, 101, 62, 62, this);

        g.setColor(new Color(140, 145, 160));
        g.drawString("1:1", 20, 92);
        g.drawString("scaled", 100, 92);
        g.drawString("missing", 210, 178);
    }
}
