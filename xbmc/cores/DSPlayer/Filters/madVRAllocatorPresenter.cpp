/*
 * (C) 2006-2014 see Authors.txt
 *
 * This file is part of MPC-HC.
 *
 * MPC-HC is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-HC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "madVRAllocatorPresenter.h"
#include "windowing/WindowingFactory.h"
#include <moreuuids.h>
#include "RendererSettings.h"
#include "guilib/GUIWindowManager.h"
#include "settings/Settings.h"
#include "settings/AdvancedSettings.h"
#include "cores/DSPlayer/Filters/MadvrSettings.h"
#include "PixelShaderList.h"
#include "DSPlayer.h"
#include "utils/log.h"

using namespace KODI::MESSAGING;

#define ShaderStage_PreScale 0
#define ShaderStage_PostScale 1

extern bool g_bExternalSubtitleTime;

//
// CmadVRAllocatorPresenter
//

CmadVRAllocatorPresenter::CmadVRAllocatorPresenter(HWND hWnd, HRESULT& hr, std::string& _Error)
  : ISubPicAllocatorPresenterImpl(hWnd, hr)
  , m_ScreenSize(0, 0)
  , m_bIsFullscreen(false)
{

  //Init Variable
  m_exclusiveCallback = ExclusiveCallback;
  m_firstBoot = true;
  m_isEnteringExclusive = false;
  m_updateDisplayLatencyForMadvr = false;
  m_kodiGuiDirtyAlgo = g_advancedSettings.m_guiAlgorithmDirtyRegions;
  m_pMadvrShared = DNew CMadvrSharedRender();
  m_activeVideoRect.SetRect(0, 0, 0, 0);
  m_frameCount = 0;
  
  if (FAILED(hr)) {
    _Error += "%s ISubPicAllocatorPresenterImpl failed\n";
    return;
  }

  hr = S_OK;
}

CmadVRAllocatorPresenter::~CmadVRAllocatorPresenter()
{
  if (m_pSRCB) {
    // nasty, but we have to let it know about our death somehow
    ((CSubRenderCallback*)(ISubRenderCallback2*)m_pSRCB)->SetDXRAP(nullptr);
  }
  
  if (m_pORCB) {
    // nasty, but we have to let it know about our death somehow
    ((COsdRenderCallback*)(IOsdRenderCallback*)m_pORCB)->SetDXRAP(nullptr);
  }

  // Unregister madVR Exclusive Callback
  if (Com::SmartQIPtr<IMadVRExclusiveModeCallback> pEXL = m_pDXR)
    pEXL->Unregister(m_exclusiveCallback, this);

  // Let's madVR restore original display mode (when adjust refresh it's handled by madVR)
  if (Com::SmartQIPtr<IMadVRCommand> pMadVrCmd = m_pDXR)
    pMadVrCmd->SendCommand("restoreDisplayModeNow");

  g_advancedSettings.m_guiAlgorithmDirtyRegions = m_kodiGuiDirtyAlgo;
  
  // the order is important here
  SAFE_DELETE(m_pMadvrShared);
  g_application.m_pPlayer->Unregister(this);
  m_pSubPicQueue = nullptr;
  m_pAllocator = nullptr;
  m_pDXR = nullptr;
  m_pORCB = nullptr;
  m_pSRCB = nullptr;

  CLog::Log(LOGDEBUG, "%s Resources released", __FUNCTION__);
}

STDMETHODIMP CmadVRAllocatorPresenter::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
  if (riid != IID_IUnknown && m_pDXR) {
    if (SUCCEEDED(m_pDXR->QueryInterface(riid, ppv))) {
      return S_OK;
    }
  }

  return __super::NonDelegatingQueryInterface(riid, ppv);
}

void CmadVRAllocatorPresenter::SetResolution()
{
  ULONGLONG frameRate;
  float fps;

  if (Com::SmartQIPtr<IMadVRInfo> pInfo = m_pDXR)
  {
    pInfo->GetUlonglong("frameRate", &frameRate);
    fps = 10000000.0 / frameRate;
  }

  SIZE nativeVideoSize = GetVideoSize(false);

  if (CSettings::GetInstance().GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF
    && (CSettings::GetInstance().GetInt(CSettings::SETTING_DSPLAYER_CHANGEREFRESHWITH) == ADJUST_REFRESHRATE_WITH_BOTH || CSettings::GetInstance().GetInt(CSettings::SETTING_DSPLAYER_CHANGEREFRESHWITH) == ADJUST_REFRESHRATE_WITH_DSPLAYER)
    && g_graphicsContext.IsFullScreenRoot())
  {
    RESOLUTION bestRes = CResolutionUtils::ChooseBestResolution(fps, nativeVideoSize.cx, false);
    CDSPlayer::SetDsWndVisible(true);
    g_graphicsContext.SetVideoResolution(bestRes);
  }
  else
    m_updateDisplayLatencyForMadvr = true;
}

void CmadVRAllocatorPresenter::ExclusiveCallback(LPVOID context, int event)
{
  CmadVRAllocatorPresenter *pThis = (CmadVRAllocatorPresenter*)context;

  std::vector<std::string> strEvent = { "IsAboutToBeEntered", "WasJustEntered", "IsAboutToBeLeft", "WasJustLeft" };

  if (event == ExclusiveModeIsAboutToBeEntered || event == ExclusiveModeIsAboutToBeLeft)
    pThis->m_isEnteringExclusive = true;

  if (event == ExclusiveModeWasJustEntered || event == ExclusiveModeWasJustLeft)
    pThis->m_isEnteringExclusive = false;

  CLog::Log(LOGDEBUG, "%s madVR %s in Fullscreen Exclusive-Mode", __FUNCTION__, strEvent[event - 1].c_str());
}

void CmadVRAllocatorPresenter::EnableExclusive(bool bEnable)
{
  if (Com::SmartQIPtr<IMadVRCommand> pMadVrCmd = m_pDXR)
    pMadVrCmd->SendCommandBool("disableExclusiveMode", !bEnable);
};

void CmadVRAllocatorPresenter::ConfigureMadvr()
{
  // Disable SeekBar
  if (Com::SmartQIPtr<IMadVRCommand> pMadVrCmd = m_pDXR)
    pMadVrCmd->SendCommandBool("disableSeekbar", true);

  // Delay Playback
  m_pSettingsManager->SetBool("delayPlaybackStart2", CSettings::GetInstance().GetBool(CSettings::SETTING_DSPLAYER_DELAYMADVRPLAYBACK));

  if (Com::SmartQIPtr<IMadVRExclusiveModeCallback> pEXL = m_pDXR)
    pEXL->Register(m_exclusiveCallback, this);

  // Exclusive Mode
  if (CSettings::GetInstance().GetBool(CSettings::SETTING_DSPLAYER_EXCLUSIVEMODE))
  {
      m_pSettingsManager->SetBool("exclusiveDelay", true);
      m_pSettingsManager->SetBool("enableExclusive", true);
  }
  else
  {
    if (Com::SmartQIPtr<IMadVRCommand> pMadVrCmd = m_pDXR)
      pMadVrCmd->SendCommandBool("disableExclusiveMode", true);
  }

  // Direct3D Mode
  int iD3DMode = CSettings::GetInstance().GetInt(CSettings::SETTING_DSPLAYER_D3DPRESNTATION);
  switch (iD3DMode)
  {
  case MADVR_D3D9:
  {
    m_pSettingsManager->SetBool("useD3d11", false);
    break;
  }
  case MADVR_D3D11_VSYNC:
  {
    m_pSettingsManager->SetBool("useD3d11", true);
    m_pSettingsManager->SetBool("d3d11noDelay", true);
    break;
  }
  case MADVR_D3D11_NOVSYNC:
  {
    m_pSettingsManager->SetBool("useD3d11", true);
    m_pSettingsManager->SetBool("d3d11noDelay", false);
    break;
  }
  }

  // Pre-Presented Frames
  int iNumPresentWindowed = CSettings::GetInstance().GetInt(CSettings::SETTING_DSPLAYER_NUMPRESENTWINDOWED);
  int iNumPresentExclusive = CSettings::GetInstance().GetInt(CSettings::SETTING_DSPLAYER_NUMPRESENTEXCLUSIVE);

  if (iNumPresentWindowed > 0)
    m_pSettingsManager->SetInt("preRenderFramesWindowed", iNumPresentWindowed);

  if (iNumPresentExclusive > 0)
    m_pSettingsManager->SetInt("preRenderFrames", iNumPresentExclusive);
}

bool CmadVRAllocatorPresenter::ParentWindowProc(HWND hWnd, UINT uMsg, WPARAM *wParam, LPARAM *lParam, LRESULT *ret)
{
  if (Com::SmartQIPtr<IMadVRSubclassReplacement> pMVRSR = m_pDXR)
    return (pMVRSR->ParentWindowProc(hWnd, uMsg, wParam, lParam, ret) != 0);
  else
    return false;
}

STDMETHODIMP CmadVRAllocatorPresenter::ClearBackground(LPCSTR name, REFERENCE_TIME frameStart, RECT *fullOutputRect, RECT *activeVideoRect)
{
  return m_pMadvrShared->Render(RENDER_LAYER_UNDER);
}

STDMETHODIMP CmadVRAllocatorPresenter::RenderOsd(LPCSTR name, REFERENCE_TIME frameStart, RECT *fullOutputRect, RECT *activeVideoRect)
{
  if (g_graphicsContext.IsFullScreenVideo())
    m_activeVideoRect.SetRect(activeVideoRect->left, activeVideoRect->top, activeVideoRect->right, activeVideoRect->bottom);

  return m_pMadvrShared->Render(RENDER_LAYER_OVER);
}

STDMETHODIMP CmadVRAllocatorPresenter::SetDeviceOsd(IDirect3DDevice9* pD3DDev)
{
  if (!pD3DDev)
  {
    // release all resources
    m_pSubPicQueue = nullptr;
    m_pAllocator = nullptr;
  }
  return S_OK;
}

HRESULT CmadVRAllocatorPresenter::SetDevice(IDirect3DDevice9* pD3DDev)
{
  CLog::Log(LOGDEBUG, "%s madVR's device it's ready", __FUNCTION__);

  if (!pD3DDev)
  {
    // release all resources
    m_pSubPicQueue = nullptr;
    m_pAllocator = nullptr;
    return S_OK;
  }

  if (m_firstBoot)
  { 
    m_pMadvrShared->CreateTextures(g_Windowing.Get3D11Device(), (IDirect3DDevice9Ex*)pD3DDev, (int)m_ScreenSize.cx, (int)m_ScreenSize.cy);

    m_firstBoot = false;
    g_advancedSettings.m_guiAlgorithmDirtyRegions = DIRTYREGION_SOLVER_FILL_VIEWPORT_ALWAYS;
  }

  Com::SmartSize size(m_ScreenSize.cx,m_ScreenSize.cy);

  if (m_pAllocator) {
    m_pAllocator->ChangeDevice(pD3DDev);
  }
  else
  {
    m_pAllocator = DNew CDX9SubPicAllocator(pD3DDev, size, true);
    if (!m_pAllocator) {
      return E_FAIL;
    }
  }

  HRESULT hr = S_OK;

  if (!m_pSubPicQueue) {
    CAutoLock cAutoLock(this);
    m_pSubPicQueue = g_dsSettings.pRendererSettings->subtitlesSettings.bufferAhead > 0
      ? (ISubPicQueue*)DNew CSubPicQueue(g_dsSettings.pRendererSettings->subtitlesSettings.bufferAhead, g_dsSettings.pRendererSettings->subtitlesSettings.disableAnimations, m_pAllocator, &hr)
      : (ISubPicQueue*)DNew CSubPicQueueNoThread(m_pAllocator, &hr);
  }
  else {
    m_pSubPicQueue->Invalidate();
  }

  if (SUCCEEDED(hr) && (m_SubPicProvider)) {
    m_pSubPicQueue->SetSubPicProvider(m_SubPicProvider);
  }

  return hr;
}

HRESULT CmadVRAllocatorPresenter::Render( REFERENCE_TIME rtStart, REFERENCE_TIME rtStop, REFERENCE_TIME atpf, int left, int top, int right, int bottom, int width, int height)
{
  Com::SmartRect wndRect(0, 0, width, height);
  Com::SmartRect videoRect(left, top, right, bottom);

  __super::SetPosition(wndRect, videoRect);

  if (!g_bExternalSubtitleTime) {
    SetTime(rtStart);
  }
  if (atpf > 0 && m_pSubPicQueue) {
    m_fps = 10000000.0 / atpf;
    m_pSubPicQueue->SetFPS(m_fps);
  }

  if (!g_application.m_pPlayer->IsRenderingVideo())
  {
    m_NativeVideoSize = GetVideoSize(false);
    m_AspectRatio = GetVideoSize(true);

    // Configure Render Manager
    g_application.m_pPlayer->Configure(m_NativeVideoSize.cx, m_NativeVideoSize.cy, m_AspectRatio.cx, m_AspectRatio.cy, m_fps, CONF_FLAGS_FULLSCREEN);
    CLog::Log(LOGDEBUG, "%s Render manager configured (FPS: %f) %i %i %i %i", __FUNCTION__, m_fps, m_NativeVideoSize.cx, m_NativeVideoSize.cy, m_AspectRatio.cx, m_AspectRatio.cy);

    // Update Display Latency for madVR (sets differents delay for each refresh as configured in advancedsettings)
    if (m_updateDisplayLatencyForMadvr)
    { 
      if (Com::SmartQIPtr<IMadVRInfo> pInfo = m_pDXR)
      { 
        double refreshRate;
        pInfo->GetDouble("refreshRate", &refreshRate);
        g_application.m_pPlayer->UpdateDisplayLatencyForMadvr(refreshRate);
      }
    }
  }

  // Reset madVR Stats at start (after 50 frames)
  if (m_frameCount > -1)
  {
    m_frameCount += 1;

    if (m_frameCount > 50)
    {
      INPUT ip;
      ip.type = INPUT_KEYBOARD;
      ip.ki.wScan = 0;
      ip.ki.time = 0;
      ip.ki.dwExtraInfo = 0;

      // Press
      ip.ki.wVk = VK_CONTROL;
      ip.ki.dwFlags = 0; // 0 for key press
      SendInput(1, &ip, sizeof(INPUT));

      ip.ki.wVk = 'R';
      ip.ki.dwFlags = 0;
      SendInput(1, &ip, sizeof(INPUT));

      // Release
      ip.ki.wVk = 'R';
      ip.ki.dwFlags = KEYEVENTF_KEYUP;
      SendInput(1, &ip, sizeof(INPUT));

      ip.ki.wVk = VK_CONTROL;
      ip.ki.dwFlags = KEYEVENTF_KEYUP;
      SendInput(1, &ip, sizeof(INPUT));

      m_frameCount = -1;
    }
  }
 
  AlphaBltSubPic(Com::SmartSize(width, height));
  return S_OK;
}

// ISubPicAllocatorPresenter
STDMETHODIMP CmadVRAllocatorPresenter::CreateRenderer(IUnknown** ppRenderer)
{
  CheckPointer(ppRenderer, E_POINTER);

  if (m_pDXR) {
    return E_UNEXPECTED;
  }

  m_pDXR.CoCreateInstance(CLSID_madVR, GetOwner());
  if (!m_pDXR) {
    return E_FAIL;
  }

  // Init Settings Manager
  m_pSettingsManager = DNew CMadvrSettingsManager(m_pDXR);

  Com::SmartQIPtr<ISubRender> pSR = m_pDXR;
  if (!pSR) {
    m_pDXR = nullptr;
    return E_FAIL;
  }

  m_pSRCB = DNew CSubRenderCallback(this);
  if (FAILED(pSR->SetCallback(m_pSRCB))) {
    m_pDXR = nullptr;
    return E_FAIL;
  }

  // IOsdRenderCallback
  Com::SmartQIPtr<IMadVROsdServices> pOR = m_pDXR;
  if (!pOR) {
    m_pDXR = nullptr;
    return E_FAIL;
  }

  m_pORCB = DNew COsdRenderCallback(this);
  if (FAILED(pOR->OsdSetRenderCallback("Kodi.Gui", m_pORCB))) {
    m_pDXR = nullptr;
    return E_FAIL;
  }

  // Configure initial Madvr Settings
  ConfigureMadvr();

  g_application.m_pPlayer->Register(this);

  (*ppRenderer = (IUnknown*)(INonDelegatingUnknown*)(this))->AddRef();

  MONITORINFO mi;
  mi.cbSize = sizeof(MONITORINFO);
  if (GetMonitorInfo(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
    m_ScreenSize.SetSize(mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    m_activeVideoRect.SetRect(0, 0, m_ScreenSize.cx, m_ScreenSize.cy);
  }

  return S_OK;
}

void CmadVRAllocatorPresenter::SetPosition(CRect sourceRect, CRect videoRect, CRect viewRect)
{
  Com::SmartRect wndR(viewRect.x1, viewRect.y1, viewRect.x2, viewRect.y2);
  Com::SmartRect videoR(videoRect.x1, videoRect.y1, videoRect.x2, videoRect.y2);
  SetPosition(wndR, videoR);
}

STDMETHODIMP_(void) CmadVRAllocatorPresenter::SetPosition(RECT w, RECT v)
{
  if (!g_graphicsContext.IsFullScreenVideo())
  {
    w.left = 0;
    w.top = 0;
    w.right = g_graphicsContext.GetWidth();
    w.bottom = g_graphicsContext.GetHeight();
  }

  RENDER_STEREO_MODE stereoMode = g_graphicsContext.GetStereoMode();
  switch (stereoMode)
  {
  case RENDER_STEREO_MODE_SPLIT_VERTICAL:
  {
    w.right *= 2;
    v.right *= 2;
    break;
  }
  case RENDER_STEREO_MODE_SPLIT_HORIZONTAL:
  {
    w.bottom *= 2;
    v.bottom *= 2;
    break;
  }
  }

  if (Com::SmartQIPtr<IBasicVideo> pBV = m_pDXR) {
    pBV->SetDefaultSourcePosition();
    pBV->SetDestinationPosition(v.left, v.top, v.right - v.left, v.bottom - v.top);
  }

  if (Com::SmartQIPtr<IVideoWindow> pVW = m_pDXR) {
    pVW->SetWindowPosition(w.left, w.top, w.right - w.left, w.bottom - w.top);
  }
}

STDMETHODIMP_(SIZE) CmadVRAllocatorPresenter::GetVideoSize(bool fCorrectAR)
{
  SIZE size = { 0, 0 };

  if (!fCorrectAR) {
    if (Com::SmartQIPtr<IBasicVideo> pBV = m_pDXR) {
      pBV->GetVideoSize(&size.cx, &size.cy);
    }
  }
  else {
    if (Com::SmartQIPtr<IBasicVideo2> pBV2 = m_pDXR) {
      pBV2->GetPreferredAspectRatio(&size.cx, &size.cy);
    }
  }

  return size;
}

STDMETHODIMP CmadVRAllocatorPresenter::GetDIB(BYTE* lpDib, DWORD* size)
{
  HRESULT hr = E_NOTIMPL;
  if (Com::SmartQIPtr<IBasicVideo> pBV = m_pDXR) {
    hr = pBV->GetCurrentImage((long*)size, (long*)lpDib);
  }
  return hr;
}

STDMETHODIMP_(bool) CmadVRAllocatorPresenter::Paint(bool fAll)
{
  return false;
}

void CmadVRAllocatorPresenter::SetPixelShader()
{
  g_dsSettings.pixelShaderList->UpdateActivatedList();
  m_shaderStage = 0;
  std::string strStage;
  PixelShaderVector& psVec = g_dsSettings.pixelShaderList->GetActivatedPixelShaders();

  for (PixelShaderVector::iterator it = psVec.begin();
    it != psVec.end(); it++)
  {
    CExternalPixelShader *Shader = *it;
    Shader->Load();
    m_shaderStage = Shader->GetStage();
    m_shaderStage == 0 ? strStage = "Pre-Resize" : strStage = "Post-Resize";
    SetPixelShader(Shader->GetSourceData().c_str(), nullptr);
    Shader->DeleteSourceData();

    CLog::Log(LOGDEBUG, "%s Set PixelShader: %s applied: %s", __FUNCTION__, Shader->GetName().c_str(), strStage.c_str());
  }
};

STDMETHODIMP CmadVRAllocatorPresenter::SetPixelShader(LPCSTR pSrcData, LPCSTR pTarget)
{
  HRESULT hr = E_NOTIMPL;
  if (Com::SmartQIPtr<IMadVRExternalPixelShaders> pEPS = m_pDXR) {
    if (!pSrcData && !pTarget) {
      hr = pEPS->ClearPixelShaders(false);
    }
    else {
      hr = pEPS->AddPixelShader(pSrcData, pTarget, m_shaderStage, nullptr);
    }
  }
  return hr;
}
