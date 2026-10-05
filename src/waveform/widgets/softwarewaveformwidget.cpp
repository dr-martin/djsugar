#include "softwarewaveformwidget.h"

#include <QPainter>

#include "moc_softwarewaveformwidget.cpp"
#include "waveform/renderers/waveformrenderbackground.h"
#include "waveform/renderers/waveformrenderbeat.h"
#include "waveform/renderers/waveformrendererendoftrack.h"
#include "waveform/renderers/waveformrendererfilteredsignal.h"
#include "waveform/renderers/waveformrendererpreroll.h"
#include "waveform/renderers/waveformrendermark.h"
#include "waveform/renderers/waveformrendermarkrange.h"

SoftwareWaveformWidget::SoftwareWaveformWidget(const QString& group,
        QWidget* parent,
        WaveformRendererSignalBase::Options options)
        : NonGLWaveformWidgetAbstract(group, parent) {
    addRenderer<WaveformRenderBackground>();
    addRenderer<WaveformRendererEndOfTrack>();
    addRenderer<WaveformRendererPreroll>();
    addRenderer<WaveformRenderMarkRange>();
    addRenderer<WaveformRendererFilteredSignal>(options);
    addRenderer<WaveformRenderBeat>();
    addRenderer<WaveformRenderMark>();

    setAttribute(Qt::WA_NoSystemBackground);
#ifdef Q_OS_ANDROID
    // DJ Sugar overlays the two software waveform widgets. Do not advertise
    // opaque painting here: the upper deck intentionally has a transparent
    // background so the lower deck remains visible.
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
#else
    setAttribute(Qt::WA_OpaquePaintEvent);
#endif

    m_initSuccess = init();
}

SoftwareWaveformWidget::~SoftwareWaveformWidget() {
}

void SoftwareWaveformWidget::castToQWidget() {
    m_widget = this;
}

void SoftwareWaveformWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    draw(&painter, event);
}
