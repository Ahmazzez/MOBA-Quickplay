#include "MistInventoryPreview.h"

#include "Components/PointLightComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"

namespace
{
constexpr float PortraitUpdateInterval = 0.1f;
constexpr int32 PortraitWidth = 512;
constexpr int32 PortraitHeight = 768;

bool IsModelPrimitive(UPrimitiveComponent* Component)
{
    if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Component))
        return Skeletal->GetSkeletalMeshAsset() != nullptr;
    if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
        return Static->GetStaticMesh() != nullptr;
    return false;
}

void GatherModelComponents(APawn* Pawn, TArray<UPrimitiveComponent*>& OutComponents)
{
    OutComponents.Reset();
    if (!IsValid(Pawn)) return;
    TArray<AActor*> Actors;
    Actors.Add(Pawn);
    Pawn->GetAttachedActors(Actors, false, true);
    for (AActor* Actor : Actors)
    {
        if (!IsValid(Actor)) continue;
        TArray<UPrimitiveComponent*> Components;
        Actor->GetComponents(Components);
        for (UPrimitiveComponent* Component : Components)
            if (IsValid(Component) && Component->IsRegistered() && IsModelPrimitive(Component))
                OutComponents.Add(Component);
    }
}

void CopyMaterials(UPrimitiveComponent* Source, UPrimitiveComponent* Copy)
{
    // Sharing the already-configured material is read-only. A weapon's dynamic
    // color changes immediately carry over without touching its gameplay mesh.
    for (int32 Index = 0; Index < Source->GetNumMaterials(); ++Index)
        if (Copy->GetMaterial(Index) != Source->GetMaterial(Index))
            Copy->SetMaterial(Index, Source->GetMaterial(Index));
}
}

AMistInventoryPreview::AMistInventoryPreview()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
    SetReplicates(false);
    SetActorEnableCollision(false);

    PreviewRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PortraitRoot"));
    RootComponent = PreviewRoot;
    ModelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PortraitModel"));
    ModelRoot->SetupAttachment(PreviewRoot);
    ModelRoot->SetRelativeRotation(FRotator(0.f, PreviewYaw, 0.f));

    Capture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("PortraitCapture"));
    Capture->SetupAttachment(PreviewRoot);
    Capture->SetAutoActivate(false);
    Capture->PrimaryComponentTick.bStartWithTickEnabled = false;
    Capture->PrimaryComponentTick.SetTickFunctionEnable(false);
    Capture->PrimaryComponentTick.bCanEverTick = false;
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    Capture->bAlwaysPersistRenderingState = false;
    Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->CaptureSource = SCS_FinalColorLDR;
    Capture->FOVAngle = 34.f;
    Capture->ShowFlags.SetAtmosphere(false);
    Capture->ShowFlags.SetFog(false);
    Capture->ShowFlags.SetVolumetricFog(false);
    Capture->ShowFlags.SetSkyLighting(false);
    Capture->ShowFlags.SetMotionBlur(false);
    Capture->ShowFlags.SetBloom(false);
    Capture->ShowFlags.SetTemporalAA(false);
    Capture->ShowFlags.SetAntiAliasing(true);
    Capture->ShowFlags.SetDynamicShadows(false);
    Capture->ShowFlags.SetGlobalIllumination(false);
    Capture->ShowFlags.SetScreenSpaceReflections(false);
    Capture->PostProcessBlendWeight = 1.f;
    Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
    Capture->PostProcessSettings.AutoExposureMethod = AEM_Manual;
    Capture->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
    Capture->PostProcessSettings.AutoExposureApplyPhysicalCameraExposure = false;
    Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
    Capture->PostProcessSettings.AutoExposureBias = 0.f;
    Capture->PostProcessSettings.bOverride_BloomIntensity = true;
    Capture->PostProcessSettings.BloomIntensity = 0.f;
    Capture->SetRelativeLocation(FVector(400.f, 0.f, 5.f));
    Capture->SetRelativeRotation(FRotator(0.f, 180.f, 0.f));

    struct FPortraitLight
    {
        const TCHAR* Name;
        FVector Position;
        FLinearColor Color;
        float Intensity;
    };
    const FPortraitLight Lights[] = {
        {TEXT("PortraitGoldKey"), FVector(240.f, -160.f, 200.f), FLinearColor(1.f, 0.91f, 0.76f), 4.f},
        {TEXT("PortraitBlueFill"), FVector(160.f, 180.f, 70.f), FLinearColor(0.66f, 0.82f, 1.f), 2.5f},
        {TEXT("PortraitIvoryRim"), FVector(-100.f, 120.f, 150.f), FLinearColor(0.92f, 0.97f, 1.f), 3.f}
    };
    for (const FPortraitLight& Light : Lights)
    {
        UPointLightComponent* Component = CreateDefaultSubobject<UPointLightComponent>(FName(Light.Name));
        Component->SetupAttachment(PreviewRoot);
        Component->SetRelativeLocation(Light.Position);
        Component->SetMobility(EComponentMobility::Movable);
        Component->SetUseInverseSquaredFalloff(false);
        Component->SetIntensity(Light.Intensity);
        Component->SetAttenuationRadius(1100.f);
        Component->SetLightColor(Light.Color);
        Component->SetCastShadows(false);
        Component->SetLightingChannels(false, false, true);
        Component->SetVisibility(false);
        StageLights.Add(Component);
    }
}

UTextureRenderTarget2D* AMistInventoryPreview::GetRenderTarget() const
{
    return RenderTarget;
}

bool AMistInventoryPreview::HasCharacterModel() const
{
    for (const FCopyBinding& Binding : Bindings)
        if (Cast<UPoseableMeshComponent>(Binding.Copy.Get())) return true;
    return false;
}

void AMistInventoryPreview::ConfigurePrimitive(UPrimitiveComponent* Primitive)
{
    Primitive->SetMobility(EComponentMobility::Movable);
    Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Primitive->SetGenerateOverlapEvents(false);
    Primitive->SetCanEverAffectNavigation(false);
    Primitive->SetCastShadow(false);
    Primitive->SetAffectDynamicIndirectLighting(false);
    Primitive->SetAffectDistanceFieldLighting(false);
    Primitive->SetReceivesDecals(false);
    Primitive->SetVisibleInSceneCaptureOnly(true);
    Primitive->SetLightingChannels(false, false, true);
    // Component registration normally enables start-enabled ticks, and
    // skinned meshes autoactivate. Pose refresh here is driven explicitly by
    // the portrait actor, so these copies must never register their own tick.
    // SetComponentTickEnabled alone is a no-op on a non-ticking static mesh.
    Primitive->SetAutoActivate(false);
    Primitive->PrimaryComponentTick.bStartWithTickEnabled = false;
    Primitive->PrimaryComponentTick.SetTickFunctionEnable(false);
    Primitive->PrimaryComponentTick.bCanEverTick = false;
}

UStaticMeshComponent* AMistInventoryPreview::CreateStagePart(const FName Name, const TCHAR* MeshPath,
    const FVector& Location, const FVector& Scale, const FLinearColor& Color)
{
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, MeshPath);
    if (!Mesh) return nullptr;
    UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this, Name);
    AddInstanceComponent(Part);
    ConfigurePrimitive(Part);
    Part->SetupAttachment(PreviewRoot);
    Part->SetStaticMesh(Mesh);
    Part->SetRelativeLocation(Location);
    Part->SetRelativeScale3D(Scale);
    Part->RegisterComponent();
    if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/LevelPrototyping/Materials/M_FlatCol.M_FlatCol")))
    {
        UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, Part);
        TArray<FMaterialParameterInfo> Parameters;
        TArray<FGuid> IDs;
        Base->GetAllVectorParameterInfo(Parameters, IDs);
        for (const FMaterialParameterInfo& Parameter : Parameters)
        {
            FString Key = Parameter.Name.ToString().ToLower();
            Key.ReplaceInline(TEXT("_"), TEXT(""));
            Key.ReplaceInline(TEXT(" "), TEXT(""));
            if (Key == TEXT("color") || Key == TEXT("basecolor"))
                Material->SetVectorParameterValue(Parameter.Name, Color);
        }
        Material->SetScalarParameterValue(TEXT("Roughness"), 0.92f);
        Material->SetScalarParameterValue(TEXT("Metallic"), 0.f);
        Part->SetMaterial(0, Material);
    }
    StageParts.Add(Part);
    Capture->ShowOnlyComponent(Part);
    return Part;
}

void AMistInventoryPreview::UpdateFromPawn(APawn* Pawn)
{
    if (Pawn != SourcePawn.Get())
    {
        if (SourcePawn.IsValid()) SourcePawn->OnDestroyed.RemoveDynamic(this, &AMistInventoryPreview::HandleSourceDestroyed);
        ClearCopies();
        SourcePawn = Pawn;
        if (IsValid(Pawn)) Pawn->OnDestroyed.AddUniqueDynamic(this, &AMistInventoryPreview::HandleSourceDestroyed);
        SecondsUntilRefresh = 0.f;
    }
    if (!IsValid(Pawn))
    {
        SetPreviewActive(false);
        return;
    }
    if (SecondsUntilRefresh <= 0.f)
    {
        RefreshSource();
        SecondsUntilRefresh = PortraitUpdateInterval;
        if (bPreviewActive) CapturePortrait();
    }
}

void AMistInventoryPreview::SetPreviewActive(const bool bActive)
{
    if (bActive == bPreviewActive) return;
    bPreviewActive = bActive;
    SetActorTickEnabled(bActive);
    for (UPointLightComponent* Light : StageLights)
        if (Light) Light->SetVisibility(bActive);
    if (!bActive) return;

    // NullRHI tests still exercise real model cloning, without allocating an
    // RHI texture or asking the unavailable renderer to capture the scene.
    if (FApp::CanEverRender() && !RenderTarget)
    {
        RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("AlphonseInventoryPortrait"), RF_Transient);
        RenderTarget->RenderTargetFormat = RTF_RGBA8;
        RenderTarget->ClearColor = FLinearColor(0.12f, 0.23f, 0.36f, 1.f);
        RenderTarget->TargetGamma = 2.2f;
        RenderTarget->bAutoGenerateMips = false;
        RenderTarget->InitAutoFormat(PortraitWidth, PortraitHeight);
        Capture->TextureTarget = RenderTarget;
    }
    if (StageParts.IsEmpty())
    {
        CreateStagePart(TEXT("PortraitBackdrop"), TEXT("/Engine/BasicShapes/Cube.Cube"),
            FVector(-170.f, 0.f, 0.f), FVector(0.1f, 14.f, 14.f), FLinearColor(0.20f, 0.34f, 0.48f));
        CreateStagePart(TEXT("PortraitIvoryPlinth"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),
            FVector(0.f, 0.f, -102.f), FVector(2.6f, 2.6f, 0.10f), FLinearColor(0.68f, 0.78f, 0.84f));
        CreateStagePart(TEXT("PortraitGoldPlinth"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),
            FVector(0.f, 0.f, -108.f), FVector(2.8f, 2.8f, 0.035f), FLinearColor(0.72f, 0.49f, 0.18f));
    }
    RefreshSource();
    SecondsUntilRefresh = PortraitUpdateInterval;
    CapturePortrait();
}

void AMistInventoryPreview::RefreshSource()
{
    APawn* Pawn = SourcePawn.Get();
    if (!IsValid(Pawn))
    {
        ClearCopies();
        SetPreviewActive(false);
        return;
    }
    TArray<UPrimitiveComponent*> Sources;
    GatherModelComponents(Pawn, Sources);
    bool bRebuild = Sources.Num() != Bindings.Num();
    if (!bRebuild)
        for (int32 Index = 0; Index < Sources.Num(); ++Index)
            if (Sources[Index] != Bindings[Index].Source.Get())
            {
                bRebuild = true;
                break;
            }
    if (bRebuild)
    {
        ClearCopies();
        for (UPrimitiveComponent* Source : Sources)
        {
            UPrimitiveComponent* Copy = nullptr;
            if (USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Source))
            {
                UPoseableMeshComponent* Pose = NewObject<UPoseableMeshComponent>(this,
                    MakeUniqueObjectName(this, UPoseableMeshComponent::StaticClass(), TEXT("PortraitSkeletalCopy")));
                Pose->SetSkinnedAssetAndUpdate(Skeletal->GetSkeletalMeshAsset());
                Copy = Pose;
            }
            else if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Source))
            {
                UStaticMeshComponent* StaticCopy = NewObject<UStaticMeshComponent>(this,
                    MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), TEXT("PortraitEquipmentCopy")));
                StaticCopy->SetStaticMesh(Static->GetStaticMesh());
                Copy = StaticCopy;
            }
            if (!Copy) continue;
            AddInstanceComponent(Copy);
            ConfigurePrimitive(Copy);
            Copy->SetupAttachment(ModelRoot);
            Copy->RegisterComponent();
            Copies.Add(Copy);
            Bindings.Add({Source, Copy});
            Capture->ShowOnlyComponent(Copy);
        }
    }
    const FTransform PawnTransform = Pawn->GetActorTransform();
    for (const FCopyBinding& Binding : Bindings)
    {
        UPrimitiveComponent* Source = Binding.Source.Get();
        UPrimitiveComponent* Copy = Binding.Copy.Get();
        if (!IsValid(Source) || !IsValid(Copy)) continue;
        Copy->SetRelativeTransform(Source->GetComponentTransform().GetRelativeTransform(PawnTransform));
        Copy->SetVisibility(Source->IsVisible() && !Source->bHiddenInGame && !Source->GetOwner()->IsHidden());
        CopyMaterials(Source, Copy);
        if (UPoseableMeshComponent* Pose = Cast<UPoseableMeshComponent>(Copy))
        {
            USkeletalMeshComponent* Skeletal = CastChecked<USkeletalMeshComponent>(Source);
            if (Pose->GetSkinnedAsset() != Skeletal->GetSkeletalMeshAsset())
                Pose->SetSkinnedAssetAndUpdate(Skeletal->GetSkeletalMeshAsset());
            // Modular armor often borrows the body's pose and never updates
            // its own local bones. Follow that leader before copying, while
            // keeping the armor's actual mesh and material on the portrait.
            USkeletalMeshComponent* PoseSource = Skeletal;
            TSet<USkeletalMeshComponent*> SeenLeaders;
            SeenLeaders.Add(PoseSource);
            while (USkeletalMeshComponent* Leader = Cast<USkeletalMeshComponent>(PoseSource->LeaderPoseComponent.Get()))
            {
                if (SeenLeaders.Contains(Leader)) break;
                SeenLeaders.Add(Leader);
                PoseSource = Leader;
            }
            Pose->CopyPoseFromSkeletalComponent(PoseSource);
            Pose->RefreshBoneTransforms();
        }
        else if (UStaticMeshComponent* StaticCopy = Cast<UStaticMeshComponent>(Copy))
        {
            const UStaticMeshComponent* StaticSource = CastChecked<UStaticMeshComponent>(Source);
            if (StaticCopy->GetStaticMesh() != StaticSource->GetStaticMesh())
                StaticCopy->SetStaticMesh(StaticSource->GetStaticMesh());
        }
    }
    RefreshFraming();
}

void AMistInventoryPreview::RefreshFraming()
{
    FBox ModelBounds(ForceInit);
    const FTransform StageTransform = GetActorTransform();
    for (UPrimitiveComponent* Copy : Copies)
        if (IsValid(Copy) && Copy->IsVisible())
            ModelBounds += Copy->Bounds.GetBox().TransformBy(StageTransform.Inverse());
    if (!ModelBounds.IsValid) return;
    const FVector Center = ModelBounds.GetCenter();
    const FVector Extent = ModelBounds.GetExtent();
    const float Tangent = FMath::Tan(FMath::DegreesToRadians(Capture->FOVAngle * 0.5f));
    const float Aspect = static_cast<float>(PortraitWidth) / PortraitHeight;
    const double Distance = FMath::Max3(275.0, Extent.Y / Tangent * 1.20f,
        Extent.Z / (Tangent / Aspect) * 1.20f) + Extent.X;
    Capture->SetRelativeLocation(Center + FVector(Distance, 0.f, 8.f));
    Capture->SetRelativeRotation((Center - Capture->GetRelativeLocation()).Rotation());
}

void AMistInventoryPreview::CapturePortrait()
{
    if (bPreviewActive && FApp::CanEverRender() && RenderTarget && HasCharacterModel())
        Capture->CaptureSceneDeferred();
}

void AMistInventoryPreview::Rotate(const float Degrees)
{
    if (!FMath::IsFinite(Degrees)) return;
    PreviewYaw = FMath::UnwindDegrees(PreviewYaw + Degrees);
    ModelRoot->SetRelativeRotation(FRotator(0.f, PreviewYaw, 0.f));
    RefreshFraming();
    CapturePortrait();
}

void AMistInventoryPreview::ResetRotation()
{
    PreviewYaw = -18.f;
    ModelRoot->SetRelativeRotation(FRotator(0.f, PreviewYaw, 0.f));
    RefreshFraming();
    CapturePortrait();
}

void AMistInventoryPreview::Tick(const float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!bPreviewActive) return;
    SecondsUntilRefresh -= DeltaSeconds;
    if (SecondsUntilRefresh <= 0.f)
    {
        RefreshSource();
        SecondsUntilRefresh = PortraitUpdateInterval;
        CapturePortrait();
    }
}

void AMistInventoryPreview::ClearCopies()
{
    for (UPrimitiveComponent* Copy : Copies)
        if (IsValid(Copy))
        {
            Capture->RemoveShowOnlyComponent(Copy);
            Copy->DestroyComponent();
        }
    Copies.Reset();
    Bindings.Reset();
}

void AMistInventoryPreview::HandleSourceDestroyed(AActor* DestroyedActor)
{
    ClearCopies();
    SourcePawn.Reset();
    SetPreviewActive(false);
}

void AMistInventoryPreview::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    SetPreviewActive(false);
    if (SourcePawn.IsValid()) SourcePawn->OnDestroyed.RemoveDynamic(this, &AMistInventoryPreview::HandleSourceDestroyed);
    SourcePawn.Reset();
    ClearCopies();
    Capture->TextureTarget = nullptr;
    Capture->ClearShowOnlyComponents();
    RenderTarget = nullptr;
    Super::EndPlay(EndPlayReason);
}
