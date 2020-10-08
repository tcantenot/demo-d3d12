#include <profiling.h>
#include <d3d12layer.h>
#include <pix3.h>
#include <microprofile.h>

using ThreadId = DWORD;

namespace Profiling
{
	int s_queueGraphics = -1;
	int s_queueCompute = -1;
	std::unordered_map<ThreadId, MicroProfileThreadLogGpu*> s_gpuThreadLog;

	int GetQueue(const D3D12_COMMAND_LIST_TYPE queueType)
	{
		switch (queueType)
		{
		case D3D12_COMMAND_LIST_TYPE_DIRECT:
			return s_queueGraphics;
		case D3D12_COMMAND_LIST_TYPE_COMPUTE:
			return s_queueCompute;
		default:
			assert(false);
		}

		return -1;
	}
}

Profiling::ScopedCpuEvent::ScopedCpuEvent(const wchar_t* groupName, const wchar_t* eventName, uint64_t color)
{
	char group[128];
	WideCharToMultiByte(CP_UTF8, 0, groupName, -1, group, 128, NULL, NULL);

	char name[128];
	WideCharToMultiByte(CP_UTF8, 0, eventName, -1, name, 128, NULL, NULL);

	m_uprofToken = MicroProfileGetToken(group, name, color, MicroProfileTokenTypeCpu);
	m_uprofTick = MicroProfileEnterInternal(m_uprofToken);
}

Profiling::ScopedCpuEvent::~ScopedCpuEvent()
{
	MicroProfileLeaveInternal(m_uprofToken, m_uprofTick);
}

Profiling::ScopedGpuEvent::ScopedGpuEvent(FCommandList* cmdList, const wchar_t* eventName, uint64_t color) :
	m_cmdList{ cmdList }
{
	ThreadId currentThreadId = GetCurrentThreadId();
	if (s_gpuThreadLog.find(currentThreadId) == s_gpuThreadLog.cend())
	{
		s_gpuThreadLog[currentThreadId] = MicroProfileThreadLogGpuAlloc();
	}

	m_uprofLog = s_gpuThreadLog[currentThreadId];

	PIXBeginEvent(cmdList->m_cmdList.get(), color, eventName);
	MICROPROFILE_GPU_BEGIN(cmdList->m_cmdList.get(), m_uprofLog);

	char name[128];
	WideCharToMultiByte(CP_UTF8, 0, eventName, -1, name, 128, NULL, NULL);

	m_uprofToken = MicroProfileGetToken("GPU", name, color, MicroProfileTokenTypeGpu);
	m_uprofTick = MicroProfileGpuEnterInternal(m_uprofLog, m_uprofToken);
}

Profiling::ScopedGpuEvent::~ScopedGpuEvent()
{
	MicroProfileGpuLeaveInternal(m_uprofLog, m_uprofToken, m_uprofTick);

	PIXEndEvent(m_cmdList->m_cmdList.get());
	uint64_t fence = MICROPROFILE_GPU_END(m_uprofLog);

	m_cmdList->m_postExecuteCallbacks.push_back(
		[fence, uprofQueue = GetQueue(m_cmdList->m_type)]()
		{
			MICROPROFILE_GPU_SUBMIT(uprofQueue, fence);
		}
	);
}

void Profiling::Initialize()
{
	s_queueGraphics = MICROPROFILE_GPU_INIT_QUEUE("GPU-Graphics-Queue");
	s_queueCompute = MICROPROFILE_GPU_INIT_QUEUE("GPU-Compute-Queue");

	MicroProfileOnThreadCreate("Main");
	MicroProfileSetEnableAllGroups(true);
	MicroProfileSetForceMetaCounters(true);
}

void Profiling::Flip()
{
	for (auto& logIt : s_gpuThreadLog)
	{
		MICROPROFILE_THREADLOGGPURESET(logIt.second);
	}

	MicroProfileFlip(0);
}