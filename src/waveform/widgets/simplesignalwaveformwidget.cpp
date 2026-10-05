#include "simplesignalwaveformwidget.h"

#include <QPainter>

#include "moc_simplesignalwaveformwidget.cpp"
#include "waveform/renderers/waveformrenderbackground.h"
#include "waveform/renderers/waveformrenderbeat.h"
#include "waveform/renderers/waveformrendererendoftrack.h"
#include "waveform/renderers/waveformrendererpreroll.h"
#include "waveform/renderers/waveformrenderersimplesignal.h"
#include "waveform/renderers/waveformrendermark.h"
#include "waveform/renderers/waveformrendermarkrange.h"

SimpleSignalWaveformWidget::SimpleSignalWaveformWidget(const QString& group, QWidget* parent)
        : NonGLWaveformWidgetAbstract(group, parent) {
    addRenderer<WaveformRenderBackground>();
    addRenderer<WaveformRendererEndOfTrack>();
    addRenderer<WaveformRendererPreroll>();
    addRenderer<WaveformRenderMarkRange>();
    addRenderer<WaveformRendererSimpleSignal>();
    addRenderer<WaveformRenderBeat>();
    addRenderer<WaveformRenderMark>();

    setAttribute(Qt::WA_NoSystemBackground);
#ifdef Q_OS_ANDROID
    // DJ Sugar overlays deck 2 on deck 1 in the shared beatmatching view.
    // Preserve alpha so the red waveform below remains visible.
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
#else
    setAttribute(Qt::WA_OpaquePaintEvent);
#endif

    m_initSuccess = init();
}

SimpleSignalWaveformWidget::~SimpleSignalWaveformWidget() {
}

void SimpleSignalWaveformWidget::castToQWidget() {
    m_widget = this;
}

void SimpleSignalWaveformWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    draw(&painter, event);
}
