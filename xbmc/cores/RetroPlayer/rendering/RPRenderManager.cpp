/*
 *      Copyright (C) 2017 Team Kodi
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this Program; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "RPRenderManager.h"
#include "RenderContext.h"
#include "RenderSettings.h"
#include "RenderTranslator.h"
#include "cores/RetroPlayer/buffers/IRenderBuffer.h"
#include "cores/RetroPlayer/buffers/IRenderBufferPool.h"
#include "cores/RetroPlayer/buffers/RenderBufferManager.h"
#include "cores/RetroPlayer/guibridge/GUIGameSettings.h"
#include "cores/RetroPlayer/guibridge/GUIRenderTargetFactory.h"
#include "cores/RetroPlayer/guibridge/IGUIRenderSettings.h"
#include "cores/RetroPlayer/process/RPProcessInfo.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPBaseRenderer.h"
#include "utils/TransformMatrix.h"
#include "messaging/ApplicationMessenger.h"
#include "threads/SingleLock.h"
#include "utils/Color.h"
#include "utils/log.h"

extern "C" {
#include "libswscale/swscale.h"
}

#include <algorithm>
#include <cstring>

using namespace KODI;
using namespace RETRO;

CRPRenderManager::CRPRenderManager(CRPProcessInfo &processInfo) :
  m_processInfo(processInfo),
  m_renderContext(processInfo.GetRenderContext()),
  m_speed(1.0),
  m_renderSettings(new CGUIGameSettings(processInfo)),
  m_renderControlFactory(new CGUIRenderTargetFactory(this))
{
}

void CRPRenderManager::Initialize()
{
  CLog::Log(LOGDEBUG, "RetroPlayer[RENDER]: Initializing render manager");
}

void CRPRenderManager::Deinitialize()
{
  CLog::Log(LOGDEBUG, "RetroPlayer[RENDER]: Deinitializing render manager");

  for (auto &pixelScaler : m_scalers)
  {
    if (pixelScaler.second != nullptr)
      sws_freeContext(pixelScaler.second);
  }
  m_scalers.clear();

  for (auto renderBuffer : m_renderBuffers)
    renderBuffer->Release();
  m_renderBuffers.clear();

  m_renderers.clear();

  m_state = RENDER_STATE::UNCONFIGURED;
}

bool CRPRenderManager::Configure(AVPixelFormat format, unsigned int nominalWidth, unsigned int nominalHeight, unsigned int maxWidth, unsigned int maxHeight)
{
  CLog::Log(LOGINFO, "RetroPlayer[RENDER]: Configuring format %s, nominal %ux%u, max %ux%u",
            CRenderTranslator::TranslatePixelFormat(format),
            nominalWidth,
            nominalHeight,
            maxWidth,
            maxHeight);

  m_format = format;
  m_maxWidth = maxWidth;
  m_maxHeight = maxHeight;
  m_width = nominalWidth; //! @todo Allow dimension changes
  m_height = nominalHeight; //! @todo Allow dimension changes

  CSingleLock lock(m_stateMutex);

  if (m_state == RENDER_STATE::UNCONFIGURED)
    m_state = RENDER_STATE::CONFIGURING;
  else
  {
    Flush();
    m_state = RENDER_STATE::RECONFIGURING;
  }

  return true;
}

void CRPRenderManager::AddFrame(const uint8_t* data, size_t size, unsigned int width, unsigned int height, unsigned int orientationDegCCW)
{
  if (m_bFlush || m_state != RENDER_STATE::CONFIGURED)
    return;

  // Validate parameters
  if (data == nullptr || size == 0 || width == 0 || height == 0)
    return;

  if (width != m_width || height != m_height)
  {
    // Reconfigure
    Configure(m_format, width, height, m_maxWidth, m_maxHeight);
    return;
  }

  // Copy frame to buffers with visible renderers
  std::vector<IRenderBuffer*> renderBuffers;
  for (IRenderBufferPool *bufferPool : m_processInfo.GetBufferManager().GetBufferPools())
  {
    if (!bufferPool->HasVisibleRenderer())
      continue;

    IRenderBuffer *renderBuffer = bufferPool->GetBuffer(size);
    if (renderBuffer != nullptr)
    {
      CopyFrame(renderBuffer, m_format, data, size, width, height);
      renderBuffers.emplace_back(renderBuffer);
    }
    else
      CLog::Log(LOGDEBUG, "RetroPlayer[RENDER]: Unable to get render buffer for frame");
  }

  {
    CSingleLock lock(m_bufferMutex);

    // Set render buffers
    for (auto renderBuffer : m_renderBuffers)
      renderBuffer->Release();
    m_renderBuffers = std::move(renderBuffers);

    // Cache frame if it arrived after being paused
    if (m_speed == 0.0)
    {
      std::vector<uint8_t> cachedFrame = std::move(m_cachedFrame);

      if (!m_bHasCachedFrame)
      {
        cachedFrame.resize(size);
        m_bHasCachedFrame = true;
      }

      if (!cachedFrame.empty())
      {
        {
          CSingleExit exit(m_bufferMutex);
          std::memcpy(cachedFrame.data(), data, size);
        }
        m_cachedFrame = std::move(cachedFrame);
      }
    }
  }
}

void CRPRenderManager::SetSpeed(double speed)
{
  m_speed = speed;
}

void CRPRenderManager::FrameMove()
{
  CheckFlush();

  bool bIsConfigured = false;

  {
    CSingleLock lock(m_stateMutex);

    if (m_state == RENDER_STATE::CONFIGURING)
    {
      MESSAGING::CApplicationMessenger::GetInstance().PostMsg(TMSG_SWITCHTOFULLSCREEN);

      m_state = RENDER_STATE::CONFIGURED;

      CLog::Log(LOGINFO, "RetroPlayer[RENDER]: Renderer configured on first frame");
    }
    else if (m_state == RENDER_STATE::RECONFIGURING)
    {
      CLog::Log(LOGDEBUG, "RetroPlayer[RENDER]: Reconfiguring %u renderer(s)", m_renderers.size());

      // Reconfigure any existing renderers
      for (auto &renderer : m_renderers)
        renderer->Configure(m_format, m_width, m_height);

      m_state = RENDER_STATE::CONFIGURED;
    }

    if (m_state == RENDER_STATE::CONFIGURED)
      bIsConfigured = true;
  }

  if (bIsConfigured)
  {
    for (auto &renderer : m_renderers)
      renderer->FrameMove();
  }
}

void CRPRenderManager::CheckFlush()
{
  if (m_bFlush)
  {
    {
      CSingleLock lock(m_bufferMutex);
      for (auto renderBuffer : m_renderBuffers)
        renderBuffer->Release();
      m_renderBuffers.clear();

      m_cachedFrame.clear();

      m_bHasCachedFrame = false;
    }

    for (const auto &renderer : m_renderers)
      renderer->Flush();

    m_processInfo.GetBufferManager().FlushPools();


    m_bFlush = false;
  }
}

void CRPRenderManager::Flush()
{
  m_bFlush = true;
}

void CRPRenderManager::TriggerUpdateResolution()
{
  m_bTriggerUpdateResolution = true;
}

void CRPRenderManager::RenderWindow(bool bClear, const RESOLUTION_INFO &coordsRes)
{
  std::shared_ptr<CRPBaseRenderer> renderer = GetRenderer(nullptr);
  if (!renderer)
    return;

  m_renderContext.SetRenderingResolution(m_renderContext.GetVideoResolution(), false);

  RenderInternal(renderer, bClear, 255);

  m_renderContext.SetRenderingResolution(coordsRes, false);
}

void CRPRenderManager::RenderControl(bool bClear, bool bUseAlpha, const CRect &renderRegion, const IGUIRenderSettings *renderSettings)
{
  std::shared_ptr<CRPBaseRenderer> renderer = GetRenderer(renderSettings);
  if (!renderer)
    return;

  // Set fullscreen
  const bool bWasFullscreen = m_renderContext.IsFullScreenVideo();
  if (bWasFullscreen)
    m_renderContext.SetFullScreenVideo(false);

  // Set coordinates
  CRect coords = renderSettings->GetDimensions();
  m_renderContext.SetViewWindow(coords.x1, coords.y1, coords.x2, coords.y2);
  TransformMatrix mat;
  m_renderContext.SetTransform(mat, 1.0, 1.0);

  // Clear render area
  if (bClear)
  {
    CRect old = m_renderContext.GetScissors();
    CRect region = renderRegion;
    region.Intersect(old);
    m_renderContext.SetScissors(region);
    m_renderContext.Clear(0);
    m_renderContext.SetScissors(old);
  }

  // Calculate alpha
  UTILS::Color alpha = 255;
  if (bUseAlpha)
    alpha = m_renderContext.MergeAlpha(0xFF000000) >> 24;

  RenderInternal(renderer, false, alpha);

  // Restore coordinates
  m_renderContext.RemoveTransform();

  // Restore fullscreen
  if (bWasFullscreen)
    m_renderContext.SetFullScreenVideo(true);
}

void CRPRenderManager::ClearBackground()
{
  m_renderContext.Clear(0);
}

bool CRPRenderManager::SupportsRenderFeature(RENDERFEATURE feature) const
{
  //! @todo Move to ProcessInfo
  for (const auto &renderer : m_renderers)
  {
    if (renderer->Supports(feature))
      return true;
  }

  return false;
}

bool CRPRenderManager::SupportsScalingMethod(SCALINGMETHOD method) const
{
  //! @todo Move to ProcessInfo
  for (IRenderBufferPool *bufferPool : m_processInfo.GetBufferManager().GetBufferPools())
  {
    CRenderVideoSettings renderSettings;
    renderSettings.SetScalingMethod(method);
    if (bufferPool->IsCompatible(renderSettings))
      return true;
  }

  return false;
}

void CRPRenderManager::RenderInternal(const std::shared_ptr<CRPBaseRenderer> &renderer, bool bClear, uint32_t alpha)
{
  renderer->PreRender(bClear);

  CSingleExit exitLock(m_renderContext.GraphicsMutex());

  IRenderBuffer *renderBuffer = GetRenderBuffer(renderer->GetBufferPool());

  // If our renderer has no buffer, try to create one from paused frame now
  if (renderBuffer == nullptr)
  {
    CreateRenderBuffer(renderer->GetBufferPool());
    renderBuffer = GetRenderBuffer(renderer->GetBufferPool());
  }

  if (renderBuffer != nullptr)
  {
    bool bUploaded = true;

    if (!renderBuffer->IsLoaded())
    {
      bUploaded = renderBuffer->UploadTexture();
      renderBuffer->SetLoaded(true);
    }

    if (bUploaded)
      renderer->SetBuffer(renderBuffer);

    renderBuffer->Release();
  }

  renderer->RenderFrame(bClear, alpha);
}

std::shared_ptr<CRPBaseRenderer> CRPRenderManager::GetRenderer(const IGUIRenderSettings *renderSettings)
{
  std::shared_ptr<CRPBaseRenderer> renderer;

  {
    CSingleLock lock(m_stateMutex);
    if (m_state == RENDER_STATE::UNCONFIGURED)
      return renderer;
  }

  CRenderSettings effectiveRenderSettings;
  effectiveRenderSettings.VideoSettings() = GetEffectiveSettings(renderSettings);

  // Check renderers in order of buffer pools
  for (IRenderBufferPool *bufferPool : m_processInfo.GetBufferManager().GetBufferPools())
  {
    renderer = GetRenderer(bufferPool, effectiveRenderSettings);
    if (renderer)
      break;
  }

  if (renderer)
  {
    renderer->SetScalingMethod(effectiveRenderSettings.VideoSettings().GetScalingMethod());
    renderer->SetViewMode(effectiveRenderSettings.VideoSettings().GetRenderViewMode());
    renderer->SetRenderRotation(effectiveRenderSettings.VideoSettings().GetRenderRotation());
  }

  return renderer;
}

std::shared_ptr<CRPBaseRenderer> CRPRenderManager::GetRenderer(IRenderBufferPool *bufferPool, const CRenderSettings &renderSettings)
{
  std::shared_ptr<CRPBaseRenderer> renderer;

  if (!bufferPool->IsCompatible(renderSettings.VideoSettings()))
  {
    CLog::Log(LOGERROR, "RetroPlayer[RENDER]: buffer pool is not compatible with renderer");
    return renderer;
  }

  // Get compatible renderer for this buffer pool
  for (const auto &it : m_renderers)
  {
    if (it->GetBufferPool() != bufferPool)
      continue;

    if (!it->IsCompatible(renderSettings.VideoSettings()))
      continue;

    renderer = it;
    break;
  }

  // If buffer pool has no compatible renderers, create one now
  if (!renderer)
  {
    CLog::Log(LOGERROR, "RetroPlayer[RENDER]: Creating renderer for %s",
              m_processInfo.GetRenderSystemName(bufferPool).c_str());

    renderer.reset(m_processInfo.CreateRenderer(bufferPool, renderSettings));
    if (renderer && renderer->Configure(m_format, m_width, m_height))
    {
      // Ensure we have a render buffer for this renderer
      CreateRenderBuffer(renderer->GetBufferPool());

      m_renderers.insert(renderer);
    }
    else
      renderer.reset();
  }

  return renderer;
}

bool CRPRenderManager::HasRenderBuffer(IRenderBufferPool *bufferPool)
{
  bool bHasRenderBuffer = false;

  CSingleLock lock(m_bufferMutex);

  auto it = std::find_if(m_renderBuffers.begin(), m_renderBuffers.end(),
    [bufferPool](IRenderBuffer *renderBuffer)
    {
      return renderBuffer->GetPool() == bufferPool;
    });

  if (it != m_renderBuffers.end())
    bHasRenderBuffer = true;

  return bHasRenderBuffer;
}

IRenderBuffer *CRPRenderManager::GetRenderBuffer(IRenderBufferPool *bufferPool)
{
  if (m_bFlush || m_state != RENDER_STATE::CONFIGURED)
    return nullptr;

  IRenderBuffer *renderBuffer = nullptr;

  CSingleLock lock(m_bufferMutex);

  auto it = std::find_if(m_renderBuffers.begin(), m_renderBuffers.end(),
    [bufferPool](IRenderBuffer *renderBuffer)
    {
      return renderBuffer->GetPool() == bufferPool;
    });

  if (it != m_renderBuffers.end())
  {
    renderBuffer = *it;
    renderBuffer->Acquire();
  }

  return renderBuffer;
}

void CRPRenderManager::CreateRenderBuffer(IRenderBufferPool *bufferPool)
{
  if (m_bFlush || m_state != RENDER_STATE::CONFIGURED)
    return;

  CSingleLock lock(m_bufferMutex);

  if (!HasRenderBuffer(bufferPool) && m_bHasCachedFrame)
  {
    std::vector<uint8_t> cachedFrame = std::move(m_cachedFrame);
    if (!cachedFrame.empty())
    {
      CLog::Log(LOGERROR, "RetroPlayer[RENDER]: Creating render buffer for renderer");

      IRenderBuffer *renderBuffer = bufferPool->GetBuffer(cachedFrame.size());
      if (renderBuffer != nullptr)
      {
        {
          CSingleExit exit(m_bufferMutex);
          CopyFrame(renderBuffer, m_format, cachedFrame.data(), cachedFrame.size(), m_width, m_height);
        }
        m_renderBuffers.emplace_back(renderBuffer);
      }
      m_cachedFrame = std::move(cachedFrame);
    }
    else
    {
      CLog::Log(LOGERROR, "RetroPlayer[RENDER]: Failed to create render buffer, no cached frame");
    }
  }
}

void CRPRenderManager::UpdateResolution()
{
  /* @todo
  if (m_bTriggerUpdateResolution)
  {
    if (m_renderContext.IsFullScreenVideo() && m_renderContext.IsFullScreenRoot())
    {
      if (CServiceBroker::GetSettings().GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF && m_fps > 0.0f)
      {
        RESOLUTION res = CResolutionUtils::ChooseBestResolution(static_cast<float>(m_framerate), 0, false);
        m_renderContext.SetVideoResolution(res);
      }
      m_bTriggerUpdateResolution = false;
      m_playerPort->VideoParamsChange();
    }
  }
  */
}

void CRPRenderManager::CopyFrame(IRenderBuffer *renderBuffer, AVPixelFormat format, const uint8_t *data, size_t size, unsigned int width, unsigned int height)
{
  const uint8_t *source = data;
  uint8_t *target = renderBuffer->GetMemory();

  if (target != nullptr)
  {
    const unsigned int sourceStride = static_cast<unsigned int>(size / height);
    const unsigned int targetStride = static_cast<unsigned int>(renderBuffer->GetFrameSize() / renderBuffer->GetHeight());

    if (m_format == renderBuffer->GetFormat())
    {
      if (sourceStride == targetStride)
        std::memcpy(target, source, size);
      else
      {
        const unsigned int widthBytes = CRenderTranslator::TranslateWidthToBytes(width, m_format);
        if (widthBytes > 0)
        {
          for (unsigned int i = 0; i < height; i++)
            std::memcpy(target + targetStride * i, source + sourceStride * i, widthBytes);
        }
      }
    }
    else
    {
      SwsContext *&scalerContext = m_scalers[renderBuffer->GetFormat()];
      scalerContext = sws_getCachedContext(scalerContext,
                                           width, height, format,
                                           renderBuffer->GetWidth(), renderBuffer->GetHeight(), renderBuffer->GetFormat(),
                                           SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

      if (scalerContext != nullptr)
      {
        uint8_t* src[] =       { const_cast<uint8_t*>(source),    nullptr,   nullptr,   nullptr };
        int      srcStride[] = { static_cast<int>(sourceStride),  0,         0,         0       };
        uint8_t *dst[] =       { target,                          nullptr,   nullptr,   nullptr };
        int      dstStride[] = { static_cast<int>(targetStride),  0,         0,         0       };

        sws_scale(scalerContext, src, srcStride, 0, height, dst, dstStride);
      }
    }
  }

  renderBuffer->ReleaseMemory();
}

CRenderVideoSettings CRPRenderManager::GetEffectiveSettings(const IGUIRenderSettings *settings) const
{
  CRenderVideoSettings effectiveSettings = m_renderSettings->GetSettings().VideoSettings();

  if (settings != nullptr)
  {
    if (settings->HasVideoFilter())
      effectiveSettings.SetVideoFilter(settings->GetSettings().VideoSettings().GetVideoFilter());
    if (settings->HasViewMode())
      effectiveSettings.SetRenderViewMode(settings->GetSettings().VideoSettings().GetRenderViewMode());
    if (settings->HasRotation())
      effectiveSettings.SetRenderRotation(settings->GetSettings().VideoSettings().GetRenderRotation());
  }

  // Sanitize settings
  if (!m_processInfo.HasScalingMethod(effectiveSettings.GetScalingMethod()))
  {
    effectiveSettings.SetScalingMethod(m_processInfo.GetDefaultScalingMethod());
  }

  return effectiveSettings;
}
