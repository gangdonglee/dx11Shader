# DX11 실사 물 렌더링 — 기술 해설

본 문서는 본 저장소가 구현한 렌더링 기법을 한 권의 교과서처럼 정리한다. 코드 위치는 `src/`와 `assets/Shaders/`를 인용한다. 수식은 머메이드/카텍스 없이 일반 마크다운으로 표기했다.

---

## 목차

1. [개요](#1-개요)
2. [렌더 파이프라인 아키텍처](#2-렌더-파이프라인-아키텍처)
3. [HDR 색공간과 ACES 톤매핑](#3-hdr-색공간과-aces-톤매핑)
4. [절차적 스카이박스 큐브맵](#4-절차적-스카이박스-큐브맵)
5. [방향성 광원과 그림자 맵](#5-방향성-광원과-그림자-맵)
6. [메시 셰이딩 — Lambert / PBR / Cel](#6-메시-셰이딩--lambert--pbr--cel)
7. [Gerstner 파동과 Phillips 스펙트럼](#7-gerstner-파동과-phillips-스펙트럼)
8. [Beer–Lambert 흡수 모델](#8-beerlambert-흡수-모델)
9. [굴절과 SceneColor 스냅숏](#9-굴절과-scenecolor-스냅숏)
10. [화면 공간 반사 (SSR)와 큐브맵 폴백](#10-화면-공간-반사-ssr와-큐브맵-폴백)
11. [코스틱과 서브서피스 스캐터](#11-코스틱과-서브서피스-스캐터)
12. [디테일 노멀과 거칠기 기반 mip 반사](#12-디테일-노멀과-거칠기-기반-mip-반사)
13. [Foam 시스템 — Jacobian, 히스토리, 노이즈](#13-foam-시스템--jacobian-히스토리-노이즈)
14. [TAA — 시간 기반 안티앨리어싱](#14-taa--시간-기반-안티앨리어싱)
15. [포스트프로세스 — 안개, 외곽선, 컬러 그레이딩](#15-포스트프로세스--안개-외곽선-컬러-그레이딩)
16. [성능 고려사항](#16-성능-고려사항)
17. [한계와 향후 개선 방향](#17-한계와-향후-개선-방향)

---

## 1. 개요

본 프로젝트는 Direct3D 11 기반의 실사풍 물 렌더링 실습 프레임워크다. 다음을 한 화면에 통합한다.

- 절차적 스카이 큐브맵 + 단일 cascade 그림자
- PBR(GGX) 또는 Cel-shading으로 그려지는 정적 메시들
- 32개 Gerstner 파동을 합성한 동적 수면
- Beer–Lambert 흡수, 굴절, 화면 공간 반사, 코스틱, 서브서피스 스캐터, 거품
- HDR 파이프라인 + ACES 톤매핑
- 단일 패스 TAA, 외곽선/안개/그레이딩 포스트프로세스

---

## 2. 렌더 파이프라인 아키텍처

한 프레임의 흐름은 [App::Run](../src/App/App.cpp)에서 다음 순서로 진행된다.

```
0  m_skybox.Update(sun)        : sun 변경 시 큐브맵 6면 재베이크 + GenerateMips
0a m_foam.Update(...)          : 1024² R8 핑퐁 RT에 Jacobian 기반 거품 누적
1  ShadowMap.BeginPass         : 2K depth target에 빛 시점에서 깊이만 렌더
   Scene.RenderShadowDepth     : 모든 메시를 ShadowVS로 깊이 패스
   ShadowMap.EndPass

2  SceneColor RT 바인딩 (R16F) : HDR off-screen target
   depth clear

3  Skybox.Render               : 풀스크린 삼각형, 큐브맵 샘플
4  Scene.Render                : 메시들 → SceneColor + depth (PBR/Cel + PCF shadow)
5  CopyResource                : SceneColor → SceneColorCopy (water가 굴절용으로 샘플)
6  Water.Render                : Gerstner + Beer-Lambert + 굴절(Copy) + SSR + cube refl
7  TAA.Resolve                 : SceneColor + History + Depth → TAAOutput
                                 (Halton 지터된 카메라로 누적, 3x3 neighborhood clamp)
8  PostProcess.Render          : TAAOutput + Depth → backbuffer
                                 (안개 → 외곽선 → ACES 톤매핑 → 그레이딩)
9  ImGui                       : backbuffer
10 Present
   TAA.Swap, prevViewProj 저장, jitter 인덱스 ++
```

**요점**: 색상은 RT(SceneColor)에서 모든 셰이딩이 누적되고, HDR 상태로 TAA를 거쳐, 톤매핑 직전까지 선형(linear) 도메인을 유지한다. 백버퍼만 LDR이다.

---

## 3. HDR 색공간과 ACES 톤매핑

### 3.1 왜 HDR인가

태양 디스크의 휘도는 표면 알베도의 100배 이상이 될 수 있다. 8-bit UNORM 백버퍼는 이를 1.0에서 강제로 클램프해 모든 강한 광원을 평탄한 흰색으로 만든다. R16G16B16A16_FLOAT 오프스크린 타깃은 대략 +/-65000의 동적 범위를 보존하므로 sun glint와 거품 specular가 "실제로 밝게" 누적된다.

[App::CreateSceneColorRT](../src/App/App.cpp):

```cpp
td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
```

큐브맵 자체도 같은 포맷으로 만들어 sun 디스크가 압축되지 않게 한다 ([Skybox::CreateCubemap](../src/Rendering/Skybox.cpp)).

### 3.2 ACES 필름 톤매핑 (Narkowicz 근사)

HDR 선형 색을 [0,1] 표시 도메인으로 압축하는 단계다. 본 프로젝트는 Krzysztof Narkowicz의 5-계수 분수식 ([Krzysztof Narkowicz, 2015](https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/))을 사용한다.

```hlsl
float3 ACESFilm(float3 x)
{
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x*(a*x + b)) / (x*(c*x + d) + e));
}
```

특성:
- 0 근처에서 선형(toe 부근만 살짝 어두움)
- 1 근처에서 부드러운 어깨(shoulder)
- 매우 큰 입력(>10)도 1.0에 점근

순서는 항상 **노출(exposure) → ACES → 채도/리프트/감마**다. ACES가 0–1로 클램프한 뒤에 색 보정을 적용해야 그레이딩이 디스플레이 도메인에서 직관적이다 ([post.hlsl](../assets/Shaders/post.hlsl)).

---

## 4. 절차적 스카이박스 큐브맵

### 4.1 6면 베이크

`Skybox`는 256×256 6면 R16F 큐브맵을 소유한다. 각 면을 위한 RTV를 생성하고, sun이 변경되면 6번 풀스크린 패스를 돌려 절차적 sky 함수를 6면에 굽는다 ([Skybox::BakeCubemap](../src/Rendering/Skybox.cpp)).

DX 큐브맵 면의 방향-↔-UV 매핑은 표준이다.

```hlsl
float3 FaceDirection(int face, float2 uv)   // uv ∈ [0,1]
{
    float2 c = uv * 2.0 - 1.0;
    if (face == 0) return normalize(float3( 1, -c.y, -c.x));   // +X
    if (face == 1) return normalize(float3(-1, -c.y,  c.x));   // -X
    if (face == 2) return normalize(float3( c.x, 1,  c.y));   // +Y
    if (face == 3) return normalize(float3( c.x,-1, -c.y));   // -Y
    if (face == 4) return normalize(float3( c.x, -c.y, 1));   // +Z
    return            normalize(float3(-c.x, -c.y,-1));       // -Z
}
```

### 4.2 절차적 sky 함수

3색 그래디언트 + sun 디스크/글로:

```
sky(d) = lerp(horizon, zenith, smoothstep₁(d.y))     if d.y >= 0
       = lerp(horizon, ground, |d.y| ramp)            otherwise
sun term = lightColor * (cosθ^64 * 0.6 + smoothstep(0.9985, 0.9998, cosθ) * 4.0)
```

여기서 `cosθ = saturate(dir · normalize(-lightDir))`. horizon/zenith/ground 색은 ImGui로 즉시 노출되며, 변경 시 dirty 플래그가 서서 다음 프레임에 자동 재베이크된다 ([Skybox::Update](../src/Rendering/Skybox.cpp)).

### 4.3 mip chain — IBL 거칠기 근사

```cpp
td.MipLevels = 0;                                  // 풀 mip 자동 할당
td.MiscFlags = MISC_TEXTURECUBE | MISC_GENERATE_MIPS;
...
ctx->GenerateMips(m_cubeSRV.Get());                // 6면 박스 필터 다운샘플
```

이상적으로는 GGX VNDF로 prefilter해야 정확한 IBL specular가 나오지만, 박스 필터 mip도 일차근사로 거칠기 효과를 낸다. 물 셰이더가 거칠기 슬라이더 값으로 mip LOD를 골라 샘플한다.

```hlsl
float lod = saturate(roughness) * MaxMip;          // MaxMip = log₂(faceSize)
float3 reflCol = SkyCube.SampleLevel(samp, R, lod).rgb;
```

거칠기 0 = 거울(mip 0), 1 = 매우 거친(top mip).

---

## 5. 방향성 광원과 그림자 맵

### 5.1 DirectionalLight

[DirectionalLight](../src/Core/DirectionalLight.h)는 (yaw, pitch, intensity, color) + 변경 감지용 `version` 카운터만 갖는 단순 구조다. setter들은 실제 값 변경 시에만 version을 올린다 → Skybox가 "필요할 때만" 재베이크.

월드 방향:

```
direction = (cos(pitch)·sin(yaw), -sin(pitch), cos(pitch)·cos(yaw))   // INTO scene
```

### 5.2 Shadow Map — 단일 cascade 직교 투영

직교 투영의 frustum은 원점 주위의 구(반지름 `m_sceneRadius`)를 모두 담도록 잡는다 ([ShadowMap::BeginPass](../src/Rendering/ShadowMap.cpp)).

```cpp
lightPos = origin - direction * (radius * 2.5);
view = LookAtLH(lightPos, origin, up);
proj = OrthoLH(2*radius, 2*radius, 0.1, 5*radius);
LightVP = view * proj;
```

깊이 텍스처는 `R32_TYPELESS` + DSV(`D32_FLOAT`) + SRV(`R32_FLOAT`)로 만든다(타입리스 + 두 view). 래스터라이저에 `DepthBias=16, SlopeScaledDepthBias=1.5`를 걸어 self-shadow acne를 줄인다.

### 5.3 PCF 3x3 + normal bias

[scene.hlsl SampleShadow](../assets/Shaders/scene.hlsl):

```hlsl
float3 biased = wpos + N * normalBias;            // 표면 법선 방향으로 살짝 오프셋
float4 ls = mul(float4(biased, 1), LightViewProj);
ls.xyz /= ls.w;
float2 uv = ls.xy * 0.5 + 0.5;  uv.y = 1 - uv.y;
float refZ = ls.z - depthBias;
// 3×3 비교 샘플링
sum = sum_{xx,yy ∈ [-1,1]} ShadowMap.SampleCmpLevelZero(ShadowCmp, uv + offset, refZ);
return sum / 9;
```

`SamplerComparisonState`는 하드웨어 PCF를 활용한다(필터링이 비교 결과에 적용). 결과 0=그림자, 1=빛. 최종 라이팅에서 NdotL 항에만 곱한다 — ambient는 그대로 살려야 그림자 안쪽이 까맣지 않다.

---

## 6. 메시 셰이딩 — Lambert / PBR / Cel

[scene.hlsl MeshPS](../assets/Shaders/scene.hlsl)는 `Params.x` 값에 따라 세 모드로 분기한다.

### 6.1 PBR (Cook-Torrance + GGX)

표준 metallic 워크플로우:

```
F0 = lerp(0.04, BaseColor, metallic)
D = a²/(π·((N·H)²·(a²-1) + 1)²),   a = roughness²        // GGX NDF
G = G_SchlickGGX(N·V) · G_SchlickGGX(N·L),  k = (r+1)²/8 // Smith
F = F0 + (1-F0)·(1 - cosθ)⁵                              // Schlick fresnel

specular = D·G·F / (4·N·V·N·L)
kd = (1 - F)·(1 - metallic)
direct = (kd·BaseColor/π + specular) · LightColor · N·L · shadow
```

ambient는 `BaseColor * Ambient * (1 - metallic*0.5)` — 금속은 확산이 적으니 ambient도 약화.

### 6.2 Cel / Toon

N·L을 4단계로 양자화한 뒤, hand-tuned 어두움/밝음 어레이로 lerp한다.

```hlsl
float CelBand(float ndl)
{
    if (ndl < 0.10) return 0.10;
    if (ndl < 0.45) return 0.40;
    if (ndl < 0.75) return 0.70;
    return 1.0;
}
float band = CelBand(NdotL) * shadow;
lit = BaseColor * lerp(coolShade, fullLit, band) * LightColor;
```

여기에 **단계적 specular 밴드**(`step(0.6, pow(NdotH, k))`)와 **rim 라이트**(`pow(1 - NdotV, 3)`)를 더해 BotW스러운 stylized 결과를 낸다.

### 6.3 Lambert (테스트용 폴백)

`BaseColor * NdotL * LightColor + BaseColor * Ambient`. 디버깅 가시성이 좋아 기본 모드가 PBR이지만 Lambert도 토글 가능.

---

## 7. Gerstner 파동과 Phillips 스펙트럼

### 7.1 단일 Gerstner 파동

각 파동은 정점 위치를 다음과 같이 변위시킨다(트로코이드 곡선).

```
P(x, z, t) = (x + Q·A·dx·cos(θ),
              z + Q·A·dz·cos(θ),
              y + A·sin(θ))
θ = w·(d·xz) - φ·t
w = 2π/L,  φ = c·w     (c = phase speed)
```

여기서 `d=(dx,dz)`는 단위 진행방향, `A`는 진폭, `L`은 파장, `Q`는 chop 강도(0=정상 사인파, 1=이상적 트로코이드).

법선은 다음 누적의 정규화로 얻는다:

```
N = normalize( -Σ dx·w·A·cos(θ),
               1 - Σ Q·w·A·sin(θ),
               -Σ dz·w·A·cos(θ) )
```

[water.hlsl GerstnerWave](../assets/Shaders/water.hlsl)가 이를 inout 누적으로 구현한다.

### 7.2 32-wave Phillips 스타일 분포

본 프로젝트는 정적 8-wave 테이블 대신 **인덱스로 파라미터를 동적 생성**한다 (FFT-lite).

```hlsl
float t = k / 32.0;
float ang = (t - 0.5)·1.5 + sin(t·12.73)·0.35 + sin(t·27.13)·0.15;
float wavelen = baseLen · lerp(2.0, 0.04, t^1.4);
float amp     = baseAmp · (wavelen / baseLen) · 0.55 · chaos(t);
float spd     = baseSpeed · sqrt(baseLen / wavelen);    // 분산 관계
```

해석:
- `wavelen`: 큰 파장(2× base)에서 작은 잔물결(0.04× base)까지 대수 분포
- `amp ∝ wavelen`: Phillips 스펙트럼 (긴 파장이 더 큼)
- `spd ∝ √(g/L) ≈ √(1/L)`: 표면파 분산 관계
- 방향: 바람축 ±0.75 rad spread + 카오스

`Q`는 모든 파동에 분배 (`qShare = Q/n`)되어 표면이 자기 교차하지 않게 한다.

---

## 8. Beer–Lambert 흡수 모델

### 8.1 물리 배경

물 분자는 파장에 따라 다른 비율로 빛을 흡수한다. 빨강이 가장 빨리, 파랑이 가장 느리게 흡수되어 깊은 바닷물이 청록·청색으로 보이는 이유다.

Beer–Lambert 법칙은 거리 `d`만큼 통과한 빛의 세기를 다음과 같이 표현한다.

```
T(λ, d) = exp(-σ(λ) · d)
```

`σ`(흡수계수, m⁻¹)는 채널마다 다르며, 본 프로젝트는 맑은 바다 측정값을 기본으로 한다.

```
σ_R = 0.45,  σ_G = 0.10,  σ_B = 0.04
```

### 8.2 굴절+산란 합성

빛이 수면을 통과해 바닥에 닿고, 산란되어 다시 수면 밖으로 나오는 경로의 총 길이는 약 `2·d`(왕복 근사). 출력색은 다음과 같이 합성된다.

```
T   = exp(-σ · d · 2)                       // RGB 투과율
out = refraction · T + scatter · (1 - T)
```

[water.hlsl WaterPS](../assets/Shaders/water.hlsl):

```hlsl
float3 floorWorld = ReconstructWorldPos(refractUV, sceneZ);
float  d   = length(i.wpos - floorWorld);
float3 T   = exp(-Extinction.xyz * d * 2.0);
float3 belowSurface = refrColor * T + Deep.rgb * (1 - T);
```

`absorbed = saturate(1 - max(T.r, T.g, T.b))`는 후속 처리(거품 마스킹, SSS thinness)에서 "물의 두께" 프록시로 재활용된다.

이 한 줄 변경이 갈색 바닥이 깊이에 따라 자연스럽게 청록→파랑으로 전환되도록 만든다 — 단순 lerp의 인공적 회색-갈색 톤이 사라진다.

---

## 9. 굴절과 SceneColor 스냅숏

### 9.1 왜 스냅숏이 필요한가

물이 자기 픽셀에서 SceneColor를 샘플하려면, 물 패스가 시작되기 *전*의 SceneColor가 필요하다. 같은 RT를 입력+출력으로 동시에 사용하면 미정의 동작이다. 따라서 매 프레임:

```
1. Scene/Skybox → SceneColor
2. CopyResource SceneColor → SceneColorCopy
3. Water draws to SceneColor, samples SceneColorCopy
```

3-자원 패턴: **활성 RT**, **읽기용 스냅숏**, 그리고 (선택적) **포스트 입력**. App에서 `m_sceneColorTex`/`m_sceneColorCopyTex` 두 텍스처를 관리한다.

### 9.2 굴절 UV 디스토션

Snell 법칙을 정확히 풀지 않고, 표면 법선의 수평 성분으로 화면 UV를 비트는 근사:

```hlsl
float refrAmt   = ExtraParams.y * (0.5 + 0.5 * (1 - NdotV));
float2 refractUV = saturate(screenUV + N.xz * refrAmt);
```

수직 시선(`NdotV ≈ 1`)에서는 디스토션이 약하고, 그래이즈 각도일수록 강해진다 — 직관적 결과.

### 9.3 sceneZ → world 재구성

코스틱과 흡수 거리를 위해 floor 픽셀의 world 위치가 필요하다.

```hlsl
float3 ReconstructWorldPos(float2 uv, float depth01)
{
    float2 ndc = uv * 2 - 1;  ndc.y = -ndc.y;
    float4 wp  = mul(float4(ndc, depth01, 1), InvViewProj);
    return wp.xyz / wp.w;
}
```

`InvViewProj`는 cbuffer로 매 프레임 전달된다.

---

## 10. 화면 공간 반사 (SSR)와 큐브맵 폴백

### 10.1 알고리즘

반사 방향 `R = reflect(-V, N)`를 따라 월드 공간에서 단계적으로 진행하면서, 각 단계의 NDC 좌표에서 SceneDepth를 샘플하고 ray 깊이와 비교한다.

```hlsl
for (s = 1..steps) {
    p = wpos + R * stepLen * s;
    sp = mul(float4(p,1), ViewProj);
    sp.xyz /= sp.w;
    uv = sp.xy * 0.5 + 0.5;  uv.y = 1 - uv.y;
    if (uv 화면 밖) break;
    sceneZ = SceneDepth.Sample(uv);
    if (sp.z >= sceneZ && sp.z < sceneZ + thickness) {
        hit color = SceneColor.Sample(uv);
        edge fade with screen distance;
        break;
    }
}
return hitCol or fallbackSky;
```

`thickness`는 "표면 두께" — ray가 표면을 살짝 지나쳤어도 hit으로 간주하는 허용오차.

### 10.2 폴백

ray가 화면 밖으로 나가거나 hit하지 못하면 큐브맵 sky를 사용한다(Section 4의 mip-LOD 샘플링 결과).

화면 가장자리 페이드는 `1 - max(|2u-1|, |2v-1|) * 1.4` 마스크로 SSR과 sky 사이를 부드럽게 전환한다.

### 10.3 한계

- 화면 밖 객체 반사 불가 (큐브맵으로 fallback)
- 두꺼운 객체의 뒷면 반사 못 잡음 (depth pyramid 없이는)
- 반사된 빛이 그림자를 받아야 정확하지만 그림자 미적용
- 거친 표면의 흐릿한 반사는 cone tracing 없이 표현 불가 → 큐브맵 mip으로만 흉내

---

## 11. 코스틱과 서브서피스 스캐터

### 11.1 절차적 코스틱

수면의 굴절 패턴이 바닥에 만드는 빛 줄무늬는 실제로 광자 매핑이나 Voronoi 셀로 계산해야 정확하지만, 비용이 너무 크다. 본 프로젝트는 두 개의 교차 사인 격자를 곱으로 합성한다.

```hlsl
float c1 = sin(p.x*1.5 + t*0.6) * sin(p.y*1.7 - t*0.5);
float c2 = sin(p.x*0.8 - t*0.3) * sin(p.y*0.9 + t*0.4);
float caustic = pow(saturate(c1² + c2²·0.5), 4);
```

`pow(x, 4)`로 임계화해 격자 모양 광점을 만든다. 굴절 색에 곱셈으로 적용하되, 마스크는 `max(T.rgb)` — 빛이 바닥까지 닿은 영역만 코스틱이 보인다.

### 11.2 서브서피스 스캐터 근사

물이 얇을 때 sun이 뒤쪽에서 비추면 표면이 청록빛으로 빛난다(파도 마루의 백라이트).

```
backLight = (V · -L + 0.4)^3                    // 뷰가 sun 방향과 반대일 때 강함
thinness  = max(T.r, T.g, T.b)                  // 투과율이 높음 = 얇은 물
sss       = backLight · thinness · Shallow · LightColor · sssStrength
```

이를 `belowSurface`에 가산한다. 깊은 물은 thinness가 0이라 자연스럽게 사라진다.

---

## 12. 디테일 노멀과 거칠기 기반 mip 반사

### 12.1 절차적 노멀맵 생성

[Water::CreateDetailNormalMap](../src/Rendering/Water.cpp)이 init 시점에 256² RGBA8 텍스처를 굽는다.

1. value-noise + fBM(5 옥타브, lacunarity=2, persistence=0.55)로 wrap-able 높이 필드 생성
2. 중앙 차분으로 normal 계산: `N = (-dx·k, -dy·k, 1) / |...|`
3. RGB = (n*0.5 + 0.5)*255 (탄젠트 공간 인코딩)
4. **A 채널 = 원본 fBM 높이** — 거품 패턴 노이즈로 재활용

### 12.2 다중 스케일 합성

PS에서 3개 레이어를 다른 월드 스케일·다른 스크롤 방향으로 샘플하고, 탄젠트 dx/dy 성분을 합산한 뒤 단위 노멀로 재구성한다.

```hlsl
float2 d1 = sample(uv * scale1 + t·v1).xy * 2 - 1;
float2 d2 = sample(uv * scale2 + t·v2).xy * 2 - 1;
float2 d3 = sample(uv * scale3 + t·v3).xy * 2 - 1;
float2 sumXY = d1 + d2 + d3;
float3 dTS = normalize(float3(sumXY.x, 1, sumXY.y));
```

`(sumXY.x, 1, sumXY.y)` 형태에 주목 — 우리 수면은 +Y 위 평면이므로 탄젠트 공간 ↔ 월드 공간 변환이 자명하다(축 swizzle만으로 표현됨).

### 12.3 Foam 영역 강도 부스트

거품 픽셀에서는 detail strength를 4배로 올린다.

```hlsl
float s = baseStrength * (1 + quickFoam * 3);
float3 perturbed = float3(Ngeo.x + dTS.x*s, Ngeo.y, Ngeo.z + dTS.z*s);
N = normalize(perturbed);
```

거품 = 무광 거친 표면이라는 직관과 일치 — 노멀이 흩어지면서 sky 반사·specular가 자동으로 산란된다.

### 12.4 mip-LOD 거칠기 반사

Section 4.3 참조. `SkyCube.SampleLevel(samp, R, lod)`에서 `lod = roughness * (mipCount-1)`. 거친 수면은 자동으로 "흐릿한 하늘" 반사를 받는다.

---

## 13. Foam 시스템 — Jacobian, 히스토리, 노이즈

거품은 가장 시각적으로 차별화되는 요소다. 본 프로젝트는 7개 기법을 결합한다.

### 13.1 Jacobian 결정자 — 부서지는 파도 검출

Gerstner 파동의 수평 변위가 Jacobian 식을 1보다 작게 만드는 영역은 표면이 자기 교차에 가까워진다 — **부서짐**. 이는 Tessendorf 해양 모델에서도 표준이다.

```
J = (1 + ∂dispₓ/∂x)·(1 + ∂dispᵤ/∂z) - (∂dispₓ/∂z)²
foam = saturate((1 - J) · gain)
```

각 Gerstner 파동마다 다음 항을 누적한다(VS에서):

```
∂dispₓ/∂x += -Q·dx²·w·sin(θ)
∂dispᵤ/∂z += -Q·dz²·w·sin(θ)
∂dispₓ/∂z += -Q·dx·dz·w·sin(θ)
```

VS는 누적 벡터 `jac.xyz`를 PS로 보간 출력하고, PS에서 행렬식을 픽셀 단위로 계산한다. 단순 "displacement Y가 크면 거품"보다 **물리적으로 정확**하다 — 매끈한 큰 파도 위는 거품이 없다.

### 13.2 Shore foam — 얕은 가장자리

`absorbed`(흡수율 ≈ 1 - max(T))가 작은 영역, 즉 매우 얕은 물에서 거품을 추가한다.

```
shoreFoam = pow(saturate(1 - absorbed*6), 4) * 0.7
```

물이 거의 흡수하지 못하는 얕은 영역에서만 활성화 → 자연스러운 해안선 거품.

### 13.3 Persistent foam map — 거품 흔적

거품이 일단 발생하면 잠시 머물러야 한다(파도 뒤의 흰 자국). [FoamMap](../src/Rendering/FoamMap.h) 클래스가 이를 처리.

- 1024² R8 ping-pong RT 두 개
- 매 프레임:
  1. 새 RT를 바인드, 이전 RT를 SRV로 묶음
  2. 풀스크린 패스가 worldXZ→UV로 매핑된 텍셀마다:
     - 동일한 32-wave Gerstner Jacobian을 재계산 (현재 거품)
     - 이전 텍셀 값에 `decay` 곱셈 (0.96 ≈ 30프레임 반감기)
     - `max(현재, 감쇠된 이전)`을 출력
  3. 다음 프레임은 RT를 swap

물 셰이더는 worldXZ에서 UV로 변환해 이 맵을 샘플하고, 픽셀 단위 Jacobian과 `max`로 합친다. 결과: 부서진 파도가 지나간 자리에 흰 자국이 남아 천천히 사라진다.

### 13.4 노이즈 패턴

단색 흰색 lerp 대신 detail normal alpha의 fBM 노이즈를 임계화해 "패치 형태" 거품을 만든다.

```hlsl
float n = sample(...).a;        // 두 레이어 mix
float threshold = 1 - foamMask;
float foam = smoothstep(threshold - 0.10, threshold + 0.05, n) * saturate(foamMask*1.4);
```

낮은 mask → 노이즈가 거의 통과 못 함 → 픽셀 거의 0
높은 mask → mask가 임계값을 끌어내려 노이즈 패턴이 드러남

### 13.5 가장자리/중앙 색 분리

```
foamCenter = (1.00, 1.00, 1.00)     // 두꺼운 거품
foamEdge   = (0.82, 0.92, 1.00)     // 얇은 거품 (서브서피스 청백)
foamColor  = lerp(foamEdge, foamCenter, smoothstep(0.3, 0.85, foam))
```

순백 페인트 같은 인공적 느낌이 사라진다.

### 13.6 Specular/fresnel 억제

거품 = roughness 1에 가까운 무광 표면. specular와 fresnel reflection은 거품 위에서 줄여야 한다.

```hlsl
spec    *= (1 - foam * 0.85);
fresnel *= (1 - foam * 0.60);
```

거품이 거울처럼 빛나지 않고 자연스러운 매트한 흰색이 된다.

---

## 14. TAA — 시간 기반 안티앨리어싱

### 14.1 Halton 지터

각 프레임 카메라 투영 행렬에 sub-pixel offset을 더해 매 프레임 다른 sub-sample 위치에서 셰이딩한다.

```cpp
int j = (frameIdx % 16) + 1;
float jx = (Halton(j, 2) - 0.5) * (2 / WIDTH);    // NDC 단위
float jy = (Halton(j, 3) - 0.5) * (2 / HEIGHT);
proj.m[2][0] += jx;     // 클립공간 X에 view.z·jx 가산 효과
proj.m[2][1] += jy;
```

Halton(2,3)은 저-discrepancy 시퀀스로, 픽셀 안에 균등 분포된 16개 sub-sample 위치를 만든다.

### 14.2 Reprojection — depth + prevVP

이전 프레임의 픽셀이 현재 어디에 있었는지 찾으려면 **세계 좌표의 동일성**을 가정한다(정적 메시).

```
worldPos = ReconstructWorldPos(curUV, depth)
prevClip = worldPos · prevVP
prevUV   = prevClip / prevClip.w → screen-space
```

수학적으로 더 간결하게: `prevClipFromCurClip = invCurVP * prevVP`을 C++에서 미리 계산해 cbuffer로 보낸다. HLSL은 그저 `mul(curClip, M)`만 한다.

### 14.3 Neighborhood clamping — 고스트 방지

```hlsl
float3 nMin = cur, nMax = cur;
for (xx, yy ∈ [-1,1]) {
    float3 c = SceneColor.Sample(uv + offset);
    nMin = min(nMin, c);  nMax = max(nMax, c);
}
nMin -= 0.125·(nMax - nMin);  nMax += 0.125·(nMax - nMin);   // 살짝 확장
hist = clamp(hist, nMin, nMax);
output = lerp(cur, hist, blend);
```

만약 reprojection이 부정확해 hist가 현재 이웃과 너무 다른 색을 가져오면(빠른 카메라 회전, 동적 객체) clamp가 그것을 안전 범위로 끌어들인다.

### 14.4 첫 프레임 처리

이전 프레임 history가 없는 init 직후엔 그냥 현재를 출력. `firstFrame` 플래그로 분기.

### 14.5 출력 처리

TAA 결과는 별도 RT(TAAOutput)에 쓴다. PostProcess는 이를 입력으로 사용. 프레임 끝에 `TAAOutput → TAAHistory` 복사로 다음 프레임 준비.

---

## 15. 포스트프로세스 — 안개, 외곽선, 컬러 그레이딩

[post.hlsl](../assets/Shaders/post.hlsl)는 단일 풀스크린 패스에 모두 묶여 있다. 순서가 중요하다.

```
1  raw color  = SceneColor (HDR)
2  apply fog (HDR)
3  apply outline (depth-Sobel)
4  exposure
5  ACES tonemap (HDR → LDR)
6  saturation, lift/gain, gamma (LDR)
```

### 15.1 거리 안개 — exponential

```
fog = saturate(1 - exp(-(dist - start) · density))
output = lerp(color, fogColor, fog)
```

`dist`는 SceneDepth로 reconstruct한 world position에서 카메라까지의 거리. sky 픽셀(depth ≈ 1)은 안개에서 제외 — 큐브맵 horizon이 이미 안개 톤을 포함하므로.

### 15.2 외곽선 — depth Sobel

```
gx = (UR + 2R + DR) - (UL + 2L + DL)
gy = (DL + 2D + DR) - (UL + 2U + UR)
g  = sqrt(gx² + gy²)
edge = saturate((g - threshold) · scaleByDistance)
```

깊이 도메인의 Sobel은 기하학적 윤곽을 잡는다(법선 변화는 못 잡음 — 별도 normal RT가 필요). 임계값을 `1 / max(1 - depth, 0.005)`로 거리 보정하면 멀리 있는 객체의 외곽선도 검출된다.

BotW 스타일 셀 셰이딩과 잘 어울린다.

### 15.3 컬러 그레이딩

Lift/Gamma/Gain은 컬러리스트가 익숙한 3-way 곡선이다.

```
color = (color + lift) · gain               // shadow lift, midtone gain
color = pow(color, 1/gamma)                 // gamma 보정
```

`saturation`은 luma 가중평균(0.299, 0.587, 0.114)과의 lerp — 0이면 흑백.

---

## 16. 성능 고려사항

### 16.1 RT 비용

| RT | 사이즈 | 포맷 | 비고 |
|---|---|---|---|
| SceneColor | 1920×1080 | R16G16B16A16F | 16 MB |
| SceneColorCopy | same | same | 16 MB |
| TAAOutput | same | same | 16 MB |
| TAAHistory | same | same | 16 MB |
| ShadowMap | 2048×2048 | R32_TYPELESS (D32) | 16 MB |
| Skybox cube | 256×256×6 mip | R16G16B16A16F | ~3 MB |
| FoamA/B | 1024×1024×2 | R8 | 2 MB |
| 합계 | | | ~85 MB |

DXGI 1080p 게임 RT 풋프린트로는 표준적이다.

### 16.2 셰이더 비용

- Water VS: 32-wave 루프 × ~60K vertex = 2M wave 평가 / 프레임. 이게 가장 무거움.
- Water PS: SSR 24-step march, 3 detail normal sample, 1 cubemap mip sample, 1 SceneColor, 1 SceneDepth. 시야 픽셀 약 1M → 약 30M sample.
- Foam update: 1024² × 32-wave 평가 = 33M wave 계산 / 프레임. Water VS만큼 무겁다.
- Skybox bake: 변경 시에만. 256² × 6 = 0.4M 픽셀.

### 16.3 최적화 여지

- Water VS에서 wave 카운트를 동적으로 (가까운 픽셀에 32, 먼 곳에 8). 현재 미구현.
- Foam update 해상도를 512²로 낮추면 4× 가속. 시각 차이 미미.
- SSR step 수를 거리에 따라 조절(near=12, far=24).
- 큐브맵을 정확한 GGX prefilter로 교체하면 mip별 cost는 동일하나 품질↑. 한 번 베이크 비용은 큼.

---

## 17. 한계와 향후 개선 방향

### 17.1 알려진 한계

1. **단일 cascade 그림자** — 먼 객체의 그림자 해상도 낮음. CSM(2~4 cascade)로 개선 가능.
2. **GGX prefilter 미구현** — 큐브맵 mip이 박스 필터라 정확한 IBL 아님. 거친 반사가 어색할 수 있음.
3. **수중 카메라 미지원** — 카메라가 물 아래로 가면 backface culling이 물을 가린다. 별도 underwater pass 필요(blue fog + 가려진 sky).
4. **동적 객체 모션 벡터 부재** — TAA가 카메라 모션만 reprojection. 움직이는 물체는 ghost 가능성.
5. **SSR depth pyramid 부재** — linear march로 step에 비례한 비용. Hi-Z SSR이 더 빠르고 정확.
6. **Foam 노멀맵 부재** — 거품 영역에서 detail normal 강도를 4× 올리지만, 별도 거품 전용 노멀이 더 자연스러울 것.
7. **단일 sun, point/spot 광원 없음** — 지역 광원, 환경 GI 없음.
8. **Shadow는 메시만 받음** — 물 표면이 그림자를 받지 않음(meshes만 ShadowVS로 그림). 해변 그림자가 물에 안 비침.

### 17.2 다음 단계

영향력 큰 순서:

1. **Water도 그림자 받기** — water.hlsl이 LightVP로 shadow map 샘플 → sun glint와 SSS가 그림자 안에서 죽음. 큰 시각 효과.
2. **CSM** — 4 cascade로 그림자 품질 향상.
3. **GPU FFT 해양** — Tessendorf 컴퓨트 셰이더로 IFFT, 4 캐스케이드. 본격 영화급 디테일.
4. **GGX prefilter 큐브맵** — Karis split-sum, 256×4 GGX importance sample 필터.
5. **모션 벡터 RT** — TAA 정확도 향상 + 모션 블러 가능.
6. **Underwater rendering** — water가 양면 + 특수 셰이더 + SSS 기반 수중 fog.
7. **HDR 환경 cubemap 로드** — DDS/HDR/EXR 임포트, IBL diffuse + specular probe.
8. **Volumetric fog** — froxel-based. 자연광 god ray.

---

## 부록 A — 파일 트리

```
src/
  App/
    main.cpp                ─ WinMain + console
    App.h/cpp               ─ 윈도우 + 메인 루프 + RT 관리
  Core/
    DX11Device.h/cpp        ─ 디바이스/스왑체인/depth target
    KeyManager.h/cpp        ─ 키보드 상태
    Camera.h/cpp            ─ Orbit camera
    DirectionalLight.h/cpp  ─ Sun (yaw/pitch/intensity/color + version)
    MathTypes.h             ─ D3DX9 호환 minimal 매트릭스
  Rendering/
    Scene.h/cpp             ─ 메시 생성 + Lambert/PBR/Cel + ShadowVS
    Skybox.h/cpp            ─ 절차적 큐브맵 베이크 + 배경 패스
    ShadowMap.h/cpp         ─ 단일 cascade 깊이 RT + light VP
    Water.h/cpp             ─ Gerstner + Beer-Lambert + 굴절 + SSR
    FoamMap.h/cpp           ─ 1024² 거품 히스토리 (ping-pong)
    PostProcess.h/cpp       ─ Fog + Outline + ACES + Grade
    TAA.h/cpp               ─ Halton 지터 + reprojection + clamp
    DebugUI.h/cpp           ─ ImGui 통합 + 프리셋

assets/Shaders/
  scene.hlsl                ─ MeshVS/MeshPS, ShadowVS, PCF
  water.hlsl                ─ WaterVS/WaterPS, SSR, Caustic, SSS, Foam
  foam.hlsl                 ─ FoamVS/FoamPS (히스토리 누적)
  skybake.hlsl              ─ 절차적 sky → cubemap face
  skybox.hlsl               ─ cubemap 배경 샘플
  taa.hlsl                  ─ TAA resolve
  post.hlsl                 ─ Fog/Outline/ACES/Grade
```

## 부록 B — 외부 참고 문헌

- Tessendorf, J. (2001). "Simulating Ocean Water." SIGGRAPH course notes.
- Karis, B. (2013). "Real Shading in Unreal Engine 4." SIGGRAPH course.
- Narkowicz, K. (2015). "ACES Filmic Tone Mapping Curve." (블로그)
- Pharr, Jakob, Humphreys. (2016). *Physically Based Rendering*. (광 전파/흡수 이론)
- Hillaire, S. (2020). "A Scalable and Production Ready Sky and Atmosphere Rendering Technique." EGSR.
- Cook, R. & Torrance, K. (1981). "A Reflectance Model for Computer Graphics."
