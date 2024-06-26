// This class provides a way to do audio processing that does not need
// to be executed in real-time. For example, broadcast encoding
// and recording encoding can be done here. This class uses double-buffering
// to increase the amount of time the CPU has to do whatever work needs to
// be done, and that work is executed in a separate thread. (Threading
// allows the next buffer to be filled while processing a buffer that's is
// already full.)

#include "engine/sidechain/enginesidechain.h"

#include <QtDebug>

#include "engine/engine.h"
#include "engine/sidechain/sidechainworker.h"
#include "moc_enginesidechain.cpp"
#include "util/counter.h"
#include "util/event.h"
#include "util/sample.h"
#include "util/trace.h"

#define SIDECHAIN_BUFFER_SIZE 65536

EngineSideChain::EngineSideChain(
        UserSettingsPointer pConfig,
        CSAMPLE* sidechainMix)
        : m_pConfig(pConfig),
          m_bStopThread(false),
          m_sampleFifo(SIDECHAIN_BUFFER_SIZE),
          m_pWorkBuffer(SampleUtil::alloc(SIDECHAIN_BUFFER_SIZE)),
          m_pSidechainMix(sidechainMix) {
    // We use HighPriority to prevent starvation by lower-priority processes (Qt
    // main thread, analysis, etc.). This used to be LowPriority but that is not
    // a suitable choice since we do semi-realtime tasks
    // in the sidechain thread. To get reliable timing, it's important
    // that this work be prioritized over the GUI and non-realtime tasks. See
    // discussion on issue #7272 and https://bugs.launchpad.net/mixxx/1.11/+bug/1194543.
    start(QThread::HighPriority);
}

EngineSideChain::~EngineSideChain() {
    m_waitLock.lock();
    m_bStopThread = true;
    m_waitForSamples.wakeAll();
    m_waitLock.unlock();

    // Wait until the thread has finished.
    wait();

    MMutexLocker locker(&m_workerLock);
    while (!m_workers.empty()) {
        SideChainWorker* pWorker = m_workers.takeLast();
        pWorker->shutdown();
        delete pWorker;
    }
    locker.unlock();

    SampleUtil::free(m_pWorkBuffer);
}

void EngineSideChain::addSideChainWorker(SideChainWorker* pWorker) {
    MMutexLocker locker(&m_workerLock);
    m_workers.append(pWorker);
}

void EngineSideChain::receiveBuffer(const AudioInput& input,
        const CSAMPLE* pBuffer,
        unsigned int iFrames) {
    VERIFY_OR_DEBUG_ASSERT(input.getType() == AudioPathType::RecordBroadcast) {
        qDebug() << "WARNING: AudioInput type is not RECORD_BROADCAST. Ignoring incoming buffer.";
        return;
    }
    // Just copy the received samples form the sound card input to the
    // engine. After processing we get it back via writeSamples()
    SampleUtil::copy(m_pSidechainMix, pBuffer, iFrames * mixxx::kEngineChannelOutputCount);
}

void EngineSideChain::writeSamples(const CSAMPLE* pBuffer, int iFrames) {
    Trace sidechain("EngineSideChain::writeSamples");
    // TODO: remove assumption of stereo buffer
    const int numSamples = iFrames * mixxx::kEngineChannelOutputCount;
    const int numSamplesWritten = m_sampleFifo.write(pBuffer, numSamples);

    if (numSamplesWritten != numSamples) {
        Counter("EngineSideChain::writeSamples buffer overrun").increment();
    }

    if (m_sampleFifo.writeAvailable() < SIDECHAIN_BUFFER_SIZE / 5) {
        // Signal to the sidechain that samples are available.
        Trace wakeup("EngineSideChain::writeSamples wake up");
        m_waitForSamples.wakeAll();
    }
}

void EngineSideChain::run() {
    // the id of this thread, for debugging purposes //XXX copypasta (should
    // factor this out somehow), -kousu 2/2009
    unsigned static id = 0;
    QThread::currentThread()->setObjectName(QString("EngineSideChain %1").arg(++id));
    static const QString tag("EngineSideChain");
    Event::start(tag);
    while (!m_bStopThread) {
        // Sleep until samples are available.
        m_waitLock.lock();

        Event::end(tag);
        m_waitForSamples.wait(&m_waitLock);
        m_waitLock.unlock();
        Event::start(tag);

        int samples_read;
        while ((samples_read = m_sampleFifo.read(m_pWorkBuffer,
                                                 SIDECHAIN_BUFFER_SIZE))) {
            Trace process("EngineSideChain::process");
            MMutexLocker locker(&m_workerLock);
            foreach (SideChainWorker* pWorker, m_workers) {
                pWorker->process(m_pWorkBuffer, samples_read);
            }
        }

        // Check to see if we're supposed to exit/stop this thread.
        if (m_bStopThread) {
            return;
        }
    }
}
