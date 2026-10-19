// An on/off switch for the ReShade tool: a rounded track with a knob that slides across, in
// the S2FM theme's purple when on. A QAbstractButton, so it is checkable, takes the keyboard and
// says toggled(bool) as a check box does; it adds no signal or slot of its own, so it needs no moc.
// The knob slides on QObject's own timer.
#pragma once

#include <QtGui/QAbstractButton>
#include <QtGui/QPainter>
#include <QtCore/QTimerEvent>

#include "rs_theme.h"

class RsSwitch : public QAbstractButton
{
public:
    explicit RsSwitch(QWidget* parent = 0) : QAbstractButton(parent), m_pos(0.0), m_timer(0)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::StrongFocus);
        setFixedSize(sizeHint());
    }

    QSize sizeHint() const { return QSize(40, 22); }

    // Without the slide, for a value that changed elsewhere.
    void setCheckedNow(bool on)
    {
        const bool was = blockSignals(true);
        setChecked(on);
        blockSignals(was);
        m_pos = on ? 1.0 : 0.0;
        if (m_timer) { killTimer(m_timer); m_timer = 0; }
        update();
    }

protected:
    void checkStateSet()
    {
        if (!m_timer) m_timer = startTimer(15);
    }
    void nextCheckState()
    {
        QAbstractButton::nextCheckState();
        checkStateSet();
    }
    void timerEvent(QTimerEvent* e)
    {
        if (e->timerId() != m_timer) { QAbstractButton::timerEvent(e); return; }
        const double target = isChecked() ? 1.0 : 0.0;
        m_pos += (target - m_pos) * 0.35;
        if (qAbs(target - m_pos) < 0.02) {
            m_pos = target;
            killTimer(m_timer);
            m_timer = 0;
        }
        update();
    }
    void paintEvent(QPaintEvent*)
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF track(1.5, 2.5, width() - 3.0, height() - 5.0);
        const QColor off = isEnabled() ? RsC(0x36, 0x29, 0x4d) : RsC(0x21, 0x1a, 0x33);
        const QColor on = isEnabled() ? RsC(0x7c, 0x3a, 0xed) : RsC(0x3d, 0x21, 0x6b);
        QColor fill;
        fill.setRgbF(off.redF() + (on.redF() - off.redF()) * m_pos, off.greenF() + (on.greenF() - off.greenF()) * m_pos,
                     off.blueF() + (on.blueF() - off.blueF()) * m_pos);
        p.setPen(hasFocus() ? QPen(RsC(0xc0, 0x84, 0xfc), 1.2) : Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
        const double d = track.height() - 4.0;
        const double x = track.left() + 2.0 + (track.width() - 4.0 - d) * m_pos;
        QColor knob = isEnabled() ? RsC(0xed, 0xe9, 0xfe) : RsC(0x63, 0x58, 0x7c);
        if (underMouse() && isEnabled()) knob = RsC(0xff, 0xff, 0xff);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 60));
        p.drawEllipse(QRectF(x, track.top() + 2.6, d, d));
        p.setBrush(knob);
        p.drawEllipse(QRectF(x, track.top() + 2.0, d, d));
    }

private:
    double m_pos;   // the knob, 0 off to 1 on
    int m_timer;
};
