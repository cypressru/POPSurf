/* The seam between the widget shells and the Component accessors.
 *
 * Widgets.java already covers what a shell remembers about itself. This covers
 * the two files agreeing about what a Component *is*: until they did, no
 * Button, TextField or Checkbox ever reached size(), setFont() or
 * getFontMetrics(), and Container.getLayout() was in neither of them. That was
 * the top result in the compatibility corpus - thirteen applets across
 * two authors on getLayout alone.
 *
 * The shape is Walter Fendt's, because his twenty applets are one template and
 * that template is the whole constituency: a Panel with a GridBagLayout, a
 * GridBagConstraints filled in and handed over once per component, a font and
 * a colour set on each through java.awt.Component, a listener on the buttons,
 * and then a paint() that draws from what the widgets say. It compiles as an
 * ordinary class as well as an applet so that `java Wire` on a host with a JDK
 * produces the reference output this is diffed against - the same method
 * libcheck.sh uses, and the reason the expectations below are observations
 * rather than opinions.
 *
 *   javac --release 8 -d applets applets/Wire.java
 *   DISPLAY=:0 java -cp applets Wire        > ref/Wire.txt
 *   ./jrun applets/Wire.class "" out.ppm 0  | sed -n 's/^applet: //p'
 *
 * Nothing here prints anything that depends on how big the applet is: a real
 * AWT applet that was never added to a browser is 0x0 and this one is 300x200,
 * and that difference is not what is under test.
 */
import java.applet.Applet;
import java.awt.*;
import java.awt.event.*;

public class Wire extends Applet implements ActionListener, ItemListener {
    Panel      panel;
    Button     go;
    TextField  field;
    Checkbox   flag;
    Choice     pick;
    Label      tag;
    Font       small;
    int        failures;
    int        actions;

    static void say(String s) {
        System.out.println(s);
    }

    void check(String what, boolean ok) {
        if(!ok)
            failures++;
        say((ok ? "ok   " : "FAIL ") + what);
    }

    /* Fendt's own helper, near enough: everything about one component in one
     * call, with the constraints object reused for the next one. Reusing it is
     * the point - setConstraints has to take a copy or every component ends up
     * sharing the last set of numbers. */
    void place(Component c, GridBagConstraints gc, int x, int y, Font f) {
        gc.gridx  = x;
        gc.gridy  = y;
        gc.fill   = GridBagConstraints.HORIZONTAL;
        gc.insets = new Insets(y, x, 1, 2);
        ((GridBagLayout)panel.getLayout()).setConstraints(c, gc);
        c.setFont(f);
        c.setBackground(Color.white);
        panel.add(c);
    }

    void build() {
        GridBagLayout      gbl = new GridBagLayout();
        GridBagConstraints gc  = new GridBagConstraints();

        small = new Font("Helvetica", Font.BOLD, 14);

        /* A container that has not been told gets the layout its class would
         * have been born with. Panel is a FlowLayout with five-pixel gaps and
         * a bare Container has none at all; both checked against a real JDK,
         * and both are answers an applet branches on. */
        Panel fresh = new Panel();
        LayoutManager born = fresh.getLayout();
        check("fresh Panel has a FlowLayout", born instanceof FlowLayout);
        check("and it is the same one every time", born == fresh.getLayout());
        check("FlowLayout's default gaps are 5",
              ((FlowLayout)born).getHgap() == 5 &&
              ((FlowLayout)born).getVgap() == 5);
        check("a bare Container has no layout",
              new Container().getLayout() == null);

        panel = new Panel();
        panel.setLayout(gbl);
        check("getLayout gives back what setLayout was given",
              panel.getLayout() == gbl);

        go    = new Button("Rechnen");
        field = new TextField("1.5", 8);
        flag  = new Checkbox("Reibung", true);
        pick  = new Choice();
        tag   = new Label("v (in m/s)", Label.RIGHT);

        pick.addItem("Kupfer");
        pick.addItem("Eisen");

        place(tag,   gc, 0, 0, small);
        place(field, gc, 1, 0, small);
        place(flag,  gc, 0, 1, small);
        place(pick,  gc, 1, 1, small);
        place(go,    gc, 0, 2, small);

        /* Every component was placed with the same constraints object, so a
         * setConstraints that kept the reference rather than a copy would
         * report the last row for the first component. */
        GridBagConstraints back = gbl.getConstraints(tag);
        check("setConstraints copies what it was given",
              back.gridx == 0 && back.gridy == 0 &&
              back.fill == GridBagConstraints.HORIZONTAL);
        check("and copies the insets with it",
              back.insets.top == 0 && back.insets.left == 0 &&
              back.insets.bottom == 1 && back.insets.right == 2);
        check("the last component kept its own",
              gbl.getConstraints(go).gridy == 2);

        /* getConstraints hands out a copy too, so writing to it changes
         * nothing. Observed on a real JDK rather than assumed. */
        back.gridx = 99;
        check("getConstraints is a copy, not the entry",
              gbl.getConstraints(tag).gridx == 0);
        check("and a fresh one each time",
              gbl.getConstraints(tag) != gbl.getConstraints(tag));
        check("a component with no constraints gets the defaults",
              gbl.getConstraints(new Button()).gridx ==
              GridBagConstraints.RELATIVE);

        add(panel);
    }

    void fonts() {
        /* setFont on a widget is the widget's own font. Three compatibility
         * applets stop on TextField.setFont and Checkbox.setFont
         * outright; they reach here through java.awt.Component, which is the
         * static type javac wrote for `Component c` above. */
        check("TextField kept the font it was given",
              field.getFont().getSize() == 14);
        check("Checkbox kept its own", flag.getFont().getSize() == 14);
        check("Button kept its own", go.getFont().getSize() == 14);

        Font big = new Font("Helvetica", Font.PLAIN, 22);
        field.setFont(big);
        check("and setting it again replaces it",
              field.getFont().getSize() == 22);
        check("without moving anybody else's",
              flag.getFont().getSize() == 14);

        check("a widget can be measured", go.getFontMetrics(small) != null);
        check("through the Component spelling",
              field.getFontMetrics(small).stringWidth("") == 0);
    }

    void boxes() {
        /* A widget that nobody laid out is 0x0, which is what a real AWT
         * component reports before its peer exists - checked on the host. The
         * applet's own box comes from the page and is a different question,
         * which is why this asks a widget and not `this`. */
        check("an unplaced widget has no size",
              go.getSize().width == 0 && go.getSize().height == 0);

        Canvas c = new Canvas();
        c.setBounds(4, 5, 60, 20);
        check("setBounds round-trips through getSize",
              c.getSize().width == 60 && c.getSize().height == 20);
        check("and through getBounds",
              c.getBounds().x == 4 && c.getBounds().y == 5 &&
              c.getBounds().width == 60);
        check("and through getLocation",
              c.getLocation().x == 4 && c.getLocation().y == 5);
        c.resize(7, 8);
        check("resize is setSize's other spelling",
              c.getSize().width == 7 && c.getBounds().x == 4);
    }

    void listeners() {
        /* Registration, which is all that can be tested here: nothing in this
         * browser can click a Button, so nothing ever fires. What matters is
         * that the call returns - it is the next thing Fendt's twenty hit once
         * the layout calls work, and seventeen groups make it. */
        go.addActionListener(this);
        field.addActionListener(this);
        flag.addItemListener(this);
        pick.addItemListener(this);
        new Scrollbar().addAdjustmentListener(new AdjustmentListener() {
            public void adjustmentValueChanged(AdjustmentEvent e) { }
        });
        check("no listener fired without an event", actions == 0);

        /* TextComponent is TextField's and TextArea's superclass, and javac
         * writes the declaring type: a field declared TextComponent calls
         * java/awt/TextComponent.setText. Fifteen groups do. */
        TextComponent tc = field;
        tc.setText("2.5");
        check("TextComponent.getText reaches the TextField",
              tc.getText().equals("2.5") && field.getText().equals("2.5"));
    }

    /* A CheckboxGroup, which used to be a constructor and nothing else: a group
     * has to hold a Checkbox and a Checkbox has to hold its group, and until
     * there was somewhere the collector could see, holding either was a
     * dangling pointer waiting for the first collection.
     *
     * Two of these are not guessable and are the reason the whole thing is
     * worth testing rather than assuming. */
    void groups() {
        CheckboxGroup grp = new CheckboxGroup();

        check("a fresh group has nothing selected",
              grp.getSelectedCheckbox() == null);

        Checkbox off = new Checkbox("off", grp, false);
        check("an unset member does not select itself",
              grp.getSelectedCheckbox() == null && !off.getState());

        Checkbox on = new Checkbox("on", grp, true);
        check("a set member selects itself",
              grp.getSelectedCheckbox() == on);

        Checkbox later = new Checkbox("later", grp, true);
        check("and the next one switches it off",
              grp.getSelectedCheckbox() == later &&
              !on.getState() && later.getState());

        grp.setSelectedCheckbox(off);
        check("setSelectedCheckbox moves the selection",
              off.getState() && !later.getState() &&
              grp.getSelectedCheckbox() == off);

        off.setState(false);
        check("the selected box cannot switch itself off", off.getState());

        on.setState(true);
        check("setState(true) on a member selects it",
              grp.getSelectedCheckbox() == on && !off.getState());

        grp.setSelectedCheckbox(null);
        check("selecting null switches off the one that was on",
              grp.getSelectedCheckbox() == null && !on.getState());

        check("getCheckboxGroup", on.getCheckboxGroup() == grp);
        check("and null for a box in no group",
              new Checkbox("lone").getCheckboxGroup() == null);
    }

    /* The part that is about the collector rather than about AWT.
     *
     * A layout and a set of constraints are the first objects this runtime
     * keeps on an applet's behalf: nothing the applet can see points at them
     * once init() returns, so if the mark phase cannot reach where they are
     * kept they are freed and the container is left pointing into the free
     * list. Under a sanitiser that is a use-after-free; on a console with no
     * memory protection it is a wrong number and, eventually, a lock-up.
     *
     * So: allocate enough to run the collector several times over, then ask
     * for everything back. The results go into locals before they are used,
     * for the reason applets/GcStack.java exists. */
    void churn() {
        GridBagLayout      gbl = new GridBagLayout();
        GridBagConstraints gc  = new GridBagConstraints();
        Button[]           kept = new Button[24];
        Panel              host = new Panel();

        host.setLayout(gbl);
        for(int i = 0; i < kept.length; i++) {
            kept[i] = new Button("k" + i);
            gc.gridx = i;
            gc.gridy = i * 2;
            gbl.setConstraints(kept[i], gc);
        }

        for(int i = 0; i < 4000; i++) {
            Button junk = new Button("j");

            junk.setFont(small);
            junk.setBounds(i, i, 3, 4);
        }

        LayoutManager back = host.getLayout();
        boolean       ok   = back == gbl;

        for(int i = 0; i < kept.length; i++) {
            GridBagConstraints c = gbl.getConstraints(kept[i]);

            if(c.gridx != i || c.gridy != i * 2 || c.insets == null)
                ok = false;
            if(!kept[i].getLabel().equals("k" + i))
                ok = false;
        }
        check("layout and constraints survive collections", ok);
    }

    public void actionPerformed(ActionEvent e) {
        actions++;
    }

    public void itemStateChanged(ItemEvent e) {
        actions++;
    }

    public void init() {
        build();
        fonts();
        boxes();
        listeners();
        groups();
        churn();
        say(failures == 0 ? "wire: all ok" : "wire: FAILURES");
    }

    /* The reference run. A real JDK executes this and the browser's runtime
     * executes init(); the two outputs are diffed. */
    public static void main(String[] a) {
        new Wire().init();
    }

    public void paint(Graphics g) {
        /* Drawn from what came back out, so a shell that quietly forgot
         * something is a wrong picture rather than a passing test. */
        String s = field.getText();
        int    n = 0;

        for(int i = 0; i < s.length(); i++)
            if(s.charAt(i) >= '0' && s.charAt(i) <= '9')
                n = n * 10 + (s.charAt(i) - '0');

        g.setColor(failures == 0 ? Color.green : Color.red);
        g.fillRect(10, 10, n * 4, field.getFont().getSize());

        g.setColor(flag.getState() ? Color.blue : Color.gray);
        g.fillRect(10, 50, ((GridBagLayout)panel.getLayout())
                           .getConstraints(go).gridy * 30, 20);

        g.setColor(Color.white);
        g.setFont(go.getFont());
        g.drawString(tag.getText() + " " + pick.getSelectedItem(), 10, 100);
    }
}
