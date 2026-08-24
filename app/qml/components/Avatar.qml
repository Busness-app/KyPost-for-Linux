import QtQuick 2.15
import com.kysecurity.mail 1.0

// STYLE_GUIDE.md §4 "Circular gradient avatar with initials" (mirrors
// web's `.users-avatar`, `.contacts-avatar`): circle, two-stop gradient
// fill from Theme.accent to Theme.accentSoft, 1px border, initials text in
// Theme.readableOnAccent. STYLE_GUIDE gives two concrete reference sizes --
// 34 for list rows, 52 for detail headers -- so `size` is a plain property
// rather than a hardcoded dimension; callers pass either.
//
// Judgment call: STYLE_GUIDE doesn't specify the border color explicitly.
// Theme.line is used here -- the same "structural outline" role it plays
// on GhostButton's stroke and PillTab's inactive border -- rather than
// inventing a new semantic color for a single use site.
//
// Judgment call: a plain QtQuick `Gradient`/`GradientStop` pair on a
// circular Rectangle (radius: width / 2) is used instead of
// Qt5Compat.GraphicalEffects / QtQuick.Effects RadialGradient or
// LinearGradient -- neither module is a project dependency (see
// app/CMakeLists.txt), and a top-to-bottom two-stop Gradient reads as a
// reasonable "gradient avatar" without adding one.
Rectangle {
    id: root

    // Public API.
    property string initials: ""
    property int size: 34
    // extended-contact-fields Task 3: a file:// URL (or "") to a cached
    // contact photo -- see ContactsController::photoPathFor(). Empty string
    // (the default) means "no photo available", which keeps the gradient +
    // initials rendering below exactly as it was before this property
    // existed; every existing caller that never sets this is unaffected.
    property string photoSource: ""

    width: root.size
    height: root.size
    radius: width / 2
    border.width: 1
    border.color: Theme.line
    // Rectangle paints its own fill/border as a genuine rounded shape
    // (that's why the gradient-only avatar below was always circular,
    // with no `clip` involved) -- but per Qt's `Item.clip` docs, `clip`
    // only ever clips an item *and its children* to the item's
    // axis-aligned bounding rectangle, never to a radius-rounded shape.
    // A plain child `Image { anchors.fill: parent }` therefore cannot be
    // clipped to this circle via `clip: true` -- it would paint a full
    // square, corners poking past the circular gradient/border. See the
    // Canvas-based photo painting below for how the photo is actually
    // clipped to the circle instead.

    gradient: Gradient {
        GradientStop { position: 0.0; color: Theme.accent }
        GradientStop { position: 1.0; color: Theme.accentSoft }
    }

    // extended-contact-fields Task 3 review fix: hidden loader for the
    // photo. Deliberately not anchored/sized -- per Image's own docs,
    // "If the width and height properties are not specified, the Image
    // automatically uses the size of the loaded image," so once
    // `status === Image.Ready`, `width`/`height` below hold the photo's
    // natural pixel dimensions, which is exactly what the aspect-crop
    // math in Canvas.onPaint needs. Never visible itself -- Canvas below
    // is what actually paints the (circularly clipped) photo.
    Image {
        id: photoImage
        source: root.photoSource
        asynchronous: true
        visible: false
        onStatusChanged: photoCanvas.requestPaint()
    }

    // Paints `photoImage` clipped to this avatar's circle. `Canvas`
    // ships with the base `QtQuick` module (no new CMake/QML-import
    // dependency, unlike Qt5Compat.GraphicalEffects / QtQuick.Effects --
    // see the judgment-call comment on the gradient above for why those
    // remain out) and its Context2D exposes real path-based clipping:
    // per the Context2D docs, `clip()` "Creates the clipping region from
    // the current path. Any parts of the shape outside the clipping path
    // are not displayed," and `drawImage()`'s own docs state the drawn
    // image "is subject to the current context clip path." So
    // beginPath()+arc()+clip() before drawImage() below produces a real
    // circular clip, unlike the removed `clip: true` above.
    Canvas {
        id: photoCanvas
        anchors.fill: parent
        visible: root.photoSource !== ""
        antialiasing: true

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();

            if (photoImage.status !== Image.Ready || photoImage.width <= 0 || photoImage.height <= 0)
                return;

            var cw = width;
            var ch = height;
            var iw = photoImage.width;
            var ih = photoImage.height;

            // Reproduce Image.PreserveAspectCrop by hand: pick the
            // largest centered source rect out of the loaded image whose
            // aspect ratio matches the canvas, then stretch that rect to
            // fill the whole canvas -- the same "scale uniformly to
            // fill, cropping if necessary" PreserveAspectCrop describes,
            // just computed manually since Context2D has no fill-mode
            // concept of its own.
            var srcAspect = iw / ih;
            var dstAspect = cw / ch;
            var sx, sy, sw, sh;
            if (srcAspect > dstAspect) {
                sh = ih;
                sw = ih * dstAspect;
                sx = (iw - sw) / 2;
                sy = 0;
            } else {
                sw = iw;
                sh = iw / dstAspect;
                sx = 0;
                sy = (ih - sh) / 2;
            }

            ctx.save();
            ctx.beginPath();
            ctx.arc(cw / 2, ch / 2, Math.min(cw, ch) / 2, 0, 2 * Math.PI, false);
            ctx.closePath();
            ctx.clip();
            ctx.drawImage(photoImage, sx, sy, sw, sh, 0, 0, cw, ch);
            ctx.restore();
        }
    }

    Text {
        anchors.centerIn: parent
        text: root.initials
        color: Theme.readableOnAccent
        font.family: Theme.fontUi
        font.pixelSize: Math.round(root.size * 0.4)
        font.weight: Font.Medium
        // Falls back to initials whenever there's no photo -- unchanged
        // behavior from before this property existed, just now made
        // explicit via a `visible` binding instead of being the only
        // possible rendering.
        visible: root.photoSource === ""
    }
}
