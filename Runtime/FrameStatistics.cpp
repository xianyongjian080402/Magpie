#include "pch.h"
#include "FrameStatistics.h"


FrameStatistics::FrameStatistics() {
    // 这两个函数不会失败
    QueryPerformanceFrequency(&m_qpcFrequency);
    QueryPerformanceCounter(&m_qpcLastTime);

    // Initialize max delta to 1/10 of a second.
    m_qpcMaxDelta = static_cast<uint64_t>(m_qpcFrequency.QuadPart / 10);
}

void FrameStatistics::ResetElapsedTime() {
    QueryPerformanceCounter(&m_qpcLastTime);

    m_framesPerSecond = 0;
    m_framesThisSecond = 0;
    m_qpcSecondCounter = 0;
}

void FrameStatistics::Tick() {
    // Query the current time.
    LARGE_INTEGER currentTime;

    QueryPerformanceCounter(&currentTime);

    uint64_t timeDelta = static_cast<uint64_t>(currentTime.QuadPart - m_qpcLastTime.QuadPart);

    m_qpcLastTime = currentTime;
    m_qpcSecondCounter += timeDelta;

    // Clamp excessively large time deltas (e.g. after paused in the debugger).
    if (timeDelta > m_qpcMaxDelta) {
        timeDelta = m_qpcMaxDelta;
    }

    // Convert QPC units into a canonical tick format. This cannot overflow due to the previous clamp.
    timeDelta *= TicksPerSecond;
    timeDelta /= static_cast<uint64_t>(m_qpcFrequency.QuadPart);

    // Variable timestep update logic.
    m_elapsedTicks = timeDelta;
    m_totalTicks += timeDelta;
    m_frameCount++;

    // Track the current framerate.
    m_framesThisSecond++;

    if (m_qpcSecondCounter >= static_cast<uint64_t>(m_qpcFrequency.QuadPart)) {
        m_framesPerSecond = m_framesThisSecond;
        m_framesThisSecond = 0;
        m_qpcSecondCounter %= static_cast<uint64_t>(m_qpcFrequency.QuadPart);

//#ifdef _DEBUG
        OutputDebugString(fmt::format(L"{} FPS\n", m_framesPerSecond).c_str());
//#endif // _DEBUG
    }
}
