/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2012-2013 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include <QtAV/AudioThread.h>
#include <private/AVThread_p.h>
#include <QtAV/AudioDecoder.h>
#include <QtAV/Packet.h>
#include <QtAV/AudioFormat.h>
#include <QtAV/AudioOutput.h>
#include <QtAV/AudioResampler.h>
#include <QtAV/AVClock.h>
#include <QtAV/QtAV_Compat.h>
#include <QtCore/QCoreApplication>

namespace QtAV {

class AudioThreadPrivate : public AVThreadPrivate
{
public:
    void init() {
        resample = false;
        last_pts = 0;
    }

    bool resample;
    qreal last_pts; //used when audio output is not available, to calculate the aproximate sleeping time
};

AudioThread::AudioThread(QObject *parent)
    :AVThread(*new AudioThreadPrivate(), parent)
{
}

/*
 *TODO:
 * if output is null or dummy, the use duration to wait
 */
void AudioThread::run()
{
    DPTR_D(AudioThread);
    //No decoder or output. No audio output is ok, just display picture
    if (!d.dec || !d.dec->isAvailable())
        return;
    resetState();
    Q_ASSERT(d.clock != 0);
    AudioDecoder *dec = static_cast<AudioDecoder*>(d.dec);
    AudioOutput *ao = static_cast<AudioOutput*>(d.writer);
    static const double max_len = 0.02; //TODO: how to choose?
    d.init();
    //TODO: bool need_sync in private class
    bool is_external_clock = d.clock->clockType() == AVClock::ExternalClock;
    Packet pkt;
    while (!d.stop) {
        processNextTask();
        //TODO: why put it at the end of loop then playNextFrame() not work?
        if (tryPause()) { //DO NOT continue, or playNextFrame() will fail
            if (d.stop)
                break; //the queue is empty and may block. should setBlocking(false) wake up cond empty?
        } else {
            if (isPaused())
                continue;
        }
        QMutexLocker locker(&d.mutex);
        Q_UNUSED(locker);
        if (d.packets.isEmpty() && !d.stop) {
            d.stop = d.demux_end;
            if (d.stop) {
                qDebug("audio queue empty and demux end. break audio thread");
                break;
            }
        }
        if (!pkt.isValid()) {
            pkt = d.packets.take(); //wait to dequeue
        }
        if (!pkt.isValid()) {
            qDebug("Invalid packet! flush audio codec context!!!!!!!! audio queue size=%d", d.packets.size());
            dec->flush();
            continue;
        }
        if (is_external_clock) {
            d.delay = pkt.pts - d.clock->value();
            /*
             *after seeking forward, a packet may be the old, v packet may be
             *the new packet, then the d.delay is very large, omit it.
             *TODO: 1. how to choose the value
             * 2. use last delay when seeking
            */
            if (qAbs(d.delay) < 2.718) {
                if (d.delay > kSyncThreshold) { //Slow down
                    //d.delay_cond.wait(&d.mutex, d.delay*1000); //replay may fail. why?
                    //qDebug("~~~~~wating for %f msecs", d.delay*1000);
                    usleep(d.delay * 1000000);
                } else if (d.delay < -kSyncThreshold) { //Speed up. drop frame?
                    //continue;
                }
            } else { //when to drop off?
                qDebug("delay %f/%f", d.delay, d.clock->value());
                if (d.delay > 0) {
                    msleep(64);
                } else {
                    //audio packet not cleaned up?
                    continue;
                }
            }
        } else {
            d.clock->updateValue(pkt.pts);
        }
        //DO NOT decode and convert if ao is not available or mute!
        bool has_ao = ao && ao->isAvailable();
        //if (!has_ao) {//do not decode?
        if (has_ao && dec->resampler()) {
            if (dec->resampler()->speed() != ao->speed()
                    || dec->resampler()->outAudioFormat() != ao->audioFormat()) {
                //resample later to ensure thread safe. TODO: test
                if (d.resample) {
                    qDebug("decoder set speed: %.2f", ao->speed());
                    dec->resampler()->setOutAudioFormat(ao->audioFormat());
                    dec->resampler()->setSpeed(ao->speed());
                    dec->resampler()->prepare();
                    d.resample = false;
                } else {
                    d.resample = true;
                }
            }
        }
        if (dec->decode(pkt.data)) {
            QByteArray decoded(dec->data());
            int decodedSize = decoded.size();
            int decodedPos = 0;
            qreal delay =0;
            //AudioFormat.durationForBytes() calculates int type internally. not accurate
            AudioFormat &af = dec->resampler()->inAudioFormat();
            qreal byte_rate = af.bytesPerSecond();
            while (decodedSize > 0) {
                int chunk = qMin(decodedSize, int(max_len*byte_rate));
                qreal chunk_delay = (qreal)chunk/(qreal)byte_rate;
                pkt.pts += chunk_delay;
                d.clock->updateDelay(delay += chunk_delay);
                QByteArray decodedChunk(chunk, 0); //volume == 0 || mute
                if (has_ao) {
                    //TODO: volume filter and other filters!!!
                    if (!ao->isMute()) {
                        decodedChunk = QByteArray::fromRawData(decoded.constData() + decodedPos, chunk);
                        qreal vol = ao->volume();
                        if (vol != 1.0) {
                            int len = decodedChunk.size()/ao->audioFormat().bytesPerSample();
                            switch (ao->audioFormat().sampleFormat()) {
                            case AudioFormat::SampleFormat_Unsigned8:
                            case AudioFormat::SampleFormat_Unsigned8Planar: {
                                quint8 *data = (quint8*)decodedChunk.data(); //TODO: other format?
                                for (int i = 0; i < len; data[i++] *= vol) {}
                            }
                                break;
                            case AudioFormat::SampleFormat_Signed16:
                            case AudioFormat::SampleFormat_Signed16Planar: {
                                qint16 *data = (qint16*)decodedChunk.data(); //TODO: other format?
                                for (int i = 0; i < len; data[i++] *= vol) {}
                            }
                                break;
                            case AudioFormat::SampleFormat_Signed32:
                            case AudioFormat::SampleFormat_Signed32Planar: {
                                qint32 *data = (qint32*)decodedChunk.data(); //TODO: other format?
                                for (int i = 0; i < len; data[i++] *= vol) {}
                            }
                                break;
                            case AudioFormat::SampleFormat_Float:
                            case AudioFormat::SampleFormat_FloatPlanar: {
                                float *data = (float*)decodedChunk.data(); //TODO: other format?
                                for (int i = 0; i < len; data[i++] *= vol) {}
                            }
                                break;
                            case AudioFormat::SampleFormat_Double:
                            case AudioFormat::SampleFormat_DoublePlanar: {
                                double *data = (double*)decodedChunk.data(); //TODO: other format?
                                for (int i = 0; i < len; data[i++] *= vol) {}
                            }
                                break;
                            default:
                                break;
                            }
                        }
                    }
                    ao->writeData(decodedChunk);
                } else {
                /*
                 * why need this even if we add delay? and usleep sounds weird
                 * the advantage is if no audio device, the play speed is ok too
                 * So is portaudio blocking the thread when playing?
                 */
                    static bool sWarn_no_ao = true; //FIXME: no warning when replay. warn only once
                    if (sWarn_no_ao) {
                        qDebug("Audio output not available! msleep(%lu)", (unsigned long)((qreal)chunk/(qreal)byte_rate * 1000));
                        sWarn_no_ao = false;
                    }
                    //TODO: avoid acummulative error. External clock?
                    msleep((unsigned long)(chunk_delay * 1000.0));
                }
                decodedPos += chunk;
                decodedSize -= chunk;
            }
            int undecoded = dec->undecodedSize();
            if (undecoded > 0) {
                pkt.data.remove(0, pkt.data.size() - undecoded);
            } else {
                pkt = Packet();
            }
        } else { //???
            qWarning("Decode audio failed");
            qreal dt = pkt.pts - d.last_pts;
            if (abs(dt) > 0.618 || dt < 0) {
                dt = 0;
            }
            //qDebug("sleep %f", dt);
            //TODO: avoid acummulative error. External clock?
            msleep((unsigned long)(dt*1000.0));
            pkt = Packet();
        }
        d.last_pts = d.clock->value(); //not pkt.pts! the delay is updated!
    }
    qDebug("Audio thread stops running...");
}

} //namespace QtAV
