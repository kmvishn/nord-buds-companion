import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';
import Clutter from 'gi://Clutter';
import Cairo from 'cairo';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

const BUS_NAME = 'com.nordbuds.Companion';
const OBJECT_PATH = '/com/nordbuds/Companion/Buds';
const INTERFACE = 'com.nordbuds.Companion.Buds';
// Owning this name tells the app to hide its AppIndicator fallback icon.
const PANEL_NAME = 'com.nordbuds.Companion.Panel';
const RING_SIZE = 64;
const RING_WIDTH = 6;

// Arc gradient ends: blue while charging, then green, amber, red by level (same as the app window).
function levelColors(level, charging) {
    if (charging)
        return [[0x1c, 0x71, 0xd8], [0x62, 0xa0, 0xea]];
    if (level >= 50)
        return [[0x26, 0xa2, 0x69], [0x57, 0xe3, 0x89]];
    if (level >= 20)
        return [[0xe5, 0xa5, 0x0a], [0xf6, 0xd3, 0x2d]];
    return [[0xc0, 0x1c, 0x28], [0xf6, 0x61, 0x51]];
}

class BatteryRing {
    constructor(name) {
        this._level = -1;
        this._charging = false;
        this.actor = new St.BoxLayout({vertical: true, style_class: 'nordbuds-col', x_expand: true});
        const ring = new St.Widget({layout_manager: new Clutter.BinLayout(), x_align: Clutter.ActorAlign.CENTER});
        this._area = new St.DrawingArea({width: RING_SIZE, height: RING_SIZE});
        this._area.connect('repaint', area => this._repaint(area));
        this._value = new St.Label({
            text: '–', style_class: 'nordbuds-value',
            x_align: Clutter.ActorAlign.CENTER, y_align: Clutter.ActorAlign.CENTER, x_expand: true, y_expand: true,
        });
        ring.add_child(this._area);
        ring.add_child(this._value);
        const title = new St.Label({text: name, style_class: 'nordbuds-name', x_align: Clutter.ActorAlign.CENTER});
        this._note = new St.Label({text: '', style_class: 'nordbuds-note', x_align: Clutter.ActorAlign.CENTER});
        for (const actor of [ring, title, this._note])
            this.actor.add_child(actor);
    }

    set(level, charging, missing) {
        const known = level >= 0;
        this._level = level;
        this._charging = charging && known;
        this._value.text = known ? `${level}%` : '–';
        const low = known && !this._charging && level < 20;
        this._note.text = this._charging ? 'Charging' : low ? 'Low' : known ? '' : missing;
        for (const [cls, on] of [['charging', this._charging], ['low', low]]) {
            if (on)
                this._note.add_style_class_name(cls);
            else
                this._note.remove_style_class_name(cls);
        }
        this._area.queue_repaint();
    }

    _repaint(area) {
        const cr = area.get_context();
        const [w, h] = area.get_surface_size();
        const fg = area.get_theme_node().get_foreground_color();
        const cx = w / 2, cy = h / 2, r = Math.min(w, h) / 2 - RING_WIDTH;

        cr.setLineWidth(RING_WIDTH);
        cr.setSourceRGBA(fg.red / 255, fg.green / 255, fg.blue / 255, 0.14);
        cr.arc(cx, cy, r, 0, 2 * Math.PI);
        cr.stroke();

        if (this._level > 0) {
            const [c1, c2] = levelColors(this._level, this._charging);
            const mix = (k, i) => (c1[i] + (c2[i] - c1[i]) * k) / 255;
            const start = -Math.PI / 2;
            const frac = Math.min(this._level, 100) / 100;
            const end = start + frac * 2 * Math.PI;
            const steps = Math.max(1, Math.round(frac * 90));
            cr.setLineCap(Cairo.LineCap.BUTT);
            for (let i = 0; i < steps; i++) {
                const k = i / steps;
                cr.setSourceRGB(mix(k, 0), mix(k, 1), mix(k, 2));
                cr.arc(cx, cy, r, start + (end - start) * k,
                    Math.min(end, start + (end - start) * (i + 1) / steps + 0.01));
                cr.stroke();
            }
            for (const [a, k] of [[start, 0], [end, 1]]) {
                cr.setSourceRGB(mix(k, 0), mix(k, 1), mix(k, 2));
                cr.arc(cx + r * Math.cos(a), cy + r * Math.sin(a), RING_WIDTH / 2, 0, 2 * Math.PI);
                cr.fill();
            }
        }
        cr.$dispose();
    }
}

const NordBudsIndicator = GObject.registerClass(
class NordBudsIndicator extends PanelMenu.Button {
    _init(extension) {
        super._init(0.5, 'Nord Buds', false);
        this._extension = extension;
        this._state = null;

        const box = new St.BoxLayout({style_class: 'panel-status-menu-box'});
        this._icon = new St.Icon({
            gicon: Gio.icon_new_for_string(GLib.build_filenamev([extension.path, 'icons', 'nordbuds-symbolic.svg'])),
            style_class: 'system-status-icon',
        });
        this._label = new St.Label({text: '', y_align: Clutter.ActorAlign.CENTER, style_class: 'nordbuds-panel-label'});
        box.add_child(this._icon);
        box.add_child(this._label);
        this.add_child(box);

        this._buildMenu();
        this.update(null);
    }

    _buildMenu() {
        this._title = new PopupMenu.PopupMenuItem('', {reactive: false});
        this.menu.addMenuItem(this._title);

        this._batteryItem = new PopupMenu.PopupBaseMenuItem({reactive: false, can_focus: false});
        const row = new St.BoxLayout({style_class: 'nordbuds-battery', x_expand: true});
        this._columns = [new BatteryRing('Left'), new BatteryRing('Right'), new BatteryRing('Case')];
        for (const column of this._columns)
            row.add_child(column.actor);
        this._batteryItem.add_child(row);
        this.menu.addMenuItem(this._batteryItem);

        this._controls = new PopupMenu.PopupMenuSection();
        this._controls.addMenuItem(new PopupMenu.PopupSeparatorMenuItem('Sound'));
        const eqItem = new PopupMenu.PopupBaseMenuItem({reactive: false, can_focus: false});
        this._eqBox = new St.BoxLayout({style_class: 'nordbuds-eq-row', x_expand: true});
        eqItem.add_child(this._eqBox);
        this._eqButtons = new Map();
        this._controls.addMenuItem(eqItem);
        this._game = new PopupMenu.PopupSwitchMenuItem('Game mode', false);
        this._game.connect('toggled', (_item, on) => this._extension.call('SetGameMode', new GLib.Variant('(b)', [on])));
        this._controls.addMenuItem(this._game);
        this._find = new PopupMenu.PopupMenuItem('Ring earbuds');
        this._find.connect('activate', () =>
            this._extension.call('Find', new GLib.Variant('(b)', [!this._state?.finding])));
        this._controls.addMenuItem(this._find);
        this.menu.addMenuItem(this._controls);

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._connectItem = new PopupMenu.PopupMenuItem('Connect');
        this._connectItem.connect('activate', () =>
            this._extension.call(this._state?.['bt-connected'] ? 'Disconnect' : 'Connect'));
        this.menu.addMenuItem(this._connectItem);
        this._openItem = new PopupMenu.PopupMenuItem('Open Nord Buds');
        this._openItem.connect('activate', () => {
            if (this._state)
                this._extension.call('OpenWindow');
            else
                this._extension.startService();
        });
        this.menu.addMenuItem(this._openItem);
    }

    _buildEq(presets) {
        for (const [id, name] of presets) {
            const button = new St.Button({label: name, style_class: 'nordbuds-eq', x_expand: true, can_focus: true});
            button.connect('clicked', () => this._extension.call('SetEq', new GLib.Variant('(i)', [id])));
            this._eqBox.add_child(button);
            this._eqButtons.set(id, button);
        }
    }

    update(s) {
        this._state = s;
        const ready = Boolean(s?.ready);

        this._label.text = s?.label ?? '';
        this._label.visible = this._label.text !== '';
        this._icon.opacity = ready ? 255 : 120;

        let title;
        if (!s)
            title = 'Nord Buds is not running';
        else if (s.bluetooth === false)
            title = 'Bluetooth is off';
        else if (s.link === 'no-device')
            title = 'No paired Nord Buds found';
        else if (!s['bt-connected'])
            title = `${s.name || 'Earbuds'}: not connected`;
        else if (!ready)
            title = `${s.name || 'Earbuds'}: connecting…`;
        else
            title = s.name || 'Nord Buds';
        this._title.label.text = title;

        this._batteryItem.visible = ready;
        this._controls.actor.visible = ready;
        if (ready) {
            this._columns[0].set(s.left, s['left-charging'], 'Not reported');
            this._columns[1].set(s.right, s['right-charging'], 'Not reported');
            this._columns[2].set(s.case, s['case-charging'], 'Open the lid');

            if (this._eqButtons.size === 0)
                this._buildEq(s['eq-presets'] ?? []);
            for (const [id, button] of this._eqButtons) {
                button.checked = id === s.eq;
                button.reactive = Boolean(s['can-eq']);
            }
            this._game.setToggleState(s['game-mode'] === 1);
            this._game.setSensitive(Boolean(s['can-game-mode']));
            this._find.label.text = s.finding ? 'Stop ringing' : 'Ring earbuds';
            this._find.setSensitive(Boolean(s['can-find']));
        }

        this._connectItem.visible = Boolean(s) && s.link !== 'no-device';
        this._connectItem.label.text = s?.['bt-connected'] ? 'Disconnect' : 'Connect';
        this._openItem.label.text = s ? 'Open Nord Buds' : 'Start Nord Buds';
    }
});

export default class NordBudsExtension extends Extension {
    enable() {
        this._indicator = null;
        this._placement = null;
        this._ownId = Gio.bus_own_name(Gio.BusType.SESSION, PANEL_NAME, Gio.BusNameOwnerFlags.NONE, null, null, null);
        this._signalId = Gio.DBus.session.signal_subscribe(BUS_NAME, INTERFACE, 'StateChanged', OBJECT_PATH, null,
            Gio.DBusSignalFlags.NONE, (_conn, _sender, _path, _iface, _signal, params) =>
                this._apply(params.recursiveUnpack()[0]));
        this._watchId = Gio.bus_watch_name(Gio.BusType.SESSION, BUS_NAME, Gio.BusNameWatcherFlags.NONE,
            () => this._fetch(), () => this._apply(null));
        this._apply(null);
    }

    disable() {
        Gio.bus_unwatch_name(this._watchId);
        this._watchId = 0;
        Gio.DBus.session.signal_unsubscribe(this._signalId);
        if (this._ownId)
            Gio.bus_unown_name(this._ownId);
        this._ownId = 0;
        this._indicator?.destroy();
        this._indicator = null;
        this._placement = null;
    }

    _fetch() {
        Gio.DBus.session.call(BUS_NAME, OBJECT_PATH, INTERFACE, 'GetState', null, new GLib.VariantType('(a{sv})'),
            Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
                try {
                    this._apply(conn.call_finish(res).recursiveUnpack()[0]);
                } catch {
                    // The name can belong to a one-off CLI run that has no state API.
                    this._apply(null);
                }
            });
    }

    _apply(state) {
        if (!this._watchId)
            return;
        try {
            this._place(state);
        } catch (e) {
            console.error(`Nord Buds: ${e.message}`);
            // Hand the top bar back to the app's AppIndicator icon rather than showing nothing.
            if (this._ownId) {
                Gio.bus_unown_name(this._ownId);
                this._ownId = 0;
            }
        }
    }

    _place(state) {
        const position = state?.['panel-position'] ?? this._placement?.position ?? 'right';
        const order = state?.['panel-order'] ?? this._placement?.order ?? 0;
        if (!this._indicator || this._placement.position !== position || this._placement.order !== order) {
            this._indicator?.destroy();
            this._indicator = new NordBudsIndicator(this);
            Main.panel.addToStatusArea(this.uuid, this._indicator, order, position);
            this._placement = {position, order};
        }
        this._indicator.update(state);
    }

    call(method, params = null) {
        Gio.DBus.session.call(BUS_NAME, OBJECT_PATH, INTERFACE, method, params, null, Gio.DBusCallFlags.NONE, -1,
            null, (conn, res) => {
                try {
                    conn.call_finish(res);
                } catch (e) {
                    console.warn(`Nord Buds: ${method} failed: ${e.message}`);
                    this._fetch();
                }
            });
    }

    startService() {
        const bin = GLib.find_program_in_path('nordbuds') ??
            GLib.build_filenamev([GLib.get_home_dir(), '.local', 'bin', 'nordbuds']);
        try {
            Gio.Subprocess.new([bin, '--background'], Gio.SubprocessFlags.NONE);
        } catch (e) {
            console.error(`Nord Buds: could not start ${bin}: ${e.message}`);
        }
    }
}
