#include "MistCombatHUD.h"
#include "MistworldPlayerController.h"
#include "MistCombatHealth.h"
#include "MistCryptSession.h"
#include "MistProgressionComponent.h"
#include "MistInventoryPreview.h"
#include "MistHUDLayout.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"

namespace
{
const FLinearColor Ink(0.045f, 0.115f, 0.19f, 1.f);
const FLinearColor Blue(0.12f, 0.34f, 0.53f, 1.f);
const FLinearColor PaleBlue(0.74f, 0.85f, 0.91f, 1.f);
const FLinearColor Ivory(0.95f, 0.955f, 0.91f, 0.98f);
const FLinearColor Gold(0.77f, 0.60f, 0.27f, 1.f);
const FLinearColor Muted(0.36f, 0.46f, 0.50f, 1.f);
const FLinearColor HealthColor(0.28f, 0.79f, 0.66f, 1.f);

// Crisp vector ornaments remain readable at different viewport sizes and need
// no external texture pack. Gameplay uses the existing keyboard controls.
struct FPainter
{
    AHUD& HUD;
    float X, Y, S;
    FPainter(AHUD& InHUD, const FMistHUDRect& Rect, float Scale)
        : HUD(InHUD), X(Rect.X), Y(Rect.Y), S(Scale) {}
    FMistHUDRect Rect(float PX, float PY, float W, float H) const
    { return {X + PX*S, Y + PY*S, W*S, H*S}; }
    void Fill(float PX, float PY, float W, float H, const FLinearColor& Color) const
    { HUD.DrawRect(Color, X + PX*S, Y + PY*S, W*S, H*S); }
    void Line(float AX, float AY, float BX, float BY, const FLinearColor& Color, float Width=1.f) const
    { HUD.DrawLine(X + AX*S, Y + AY*S, X + BX*S, Y + BY*S, Color, Width*S); }
    void Text(const FString& Value, float PX, float PY, float Size, const FLinearColor& Color,
        float MaxWidth=1000.f, bool Center=false, bool Title=false) const
    {
        UFont* Font = Title ? GEngine->GetMediumFont() : GEngine->GetSmallFont();
        float W=0.f, H=0.f, FontScale=Size*S;
        HUD.GetTextSize(Value, W, H, Font, FontScale);
        if (W > MaxWidth*S && W > 0.f)
        { FontScale *= MaxWidth*S/W; W = MaxWidth*S; }
        HUD.DrawText(Value, Color, X + PX*S - (Center ? W*0.5f : 0.f), Y + PY*S, Font, FontScale);
    }
    void Wrapped(const FString& Value, float PX, float PY, float MaxWidth, float Size,
        const FLinearColor& Color, int32 MaxLines=3) const
    {
        TArray<FString> Words; Value.ParseIntoArrayWS(Words);
        FString Current; int32 Row=0;
        for (const FString& Word : Words)
        {
            const FString Candidate=Current.IsEmpty() ? Word : Current + TEXT(" ") + Word;
            float W=0.f,H=0.f;
            HUD.GetTextSize(Candidate,W,H,GEngine->GetSmallFont(),Size*S);
            if (W > MaxWidth*S && !Current.IsEmpty() && Row < MaxLines-1)
            { Text(Current,PX,PY+Row*21.f,Size,Color,MaxWidth); ++Row; Current=Word; }
            else Current=Candidate;
        }
        Text(Current,PX,PY+Row*21.f,Size,Color,MaxWidth);
    }
    void Frame(float PX, float PY, float W, float H, const FLinearColor& Color=Ivory,
        const FLinearColor& Edge=Gold, bool Shadow=true) const
    {
        if (Shadow) Fill(PX+3.f,PY+5.f,W,H,FLinearColor(0.015f,0.04f,0.07f,0.24f));
        Fill(PX,PY,W,H,Color);
        Line(PX,PY,PX+W,PY,Edge); Line(PX,PY+H,PX+W,PY+H,Edge);
        Line(PX,PY,PX,PY+H,Edge); Line(PX+W,PY,PX+W,PY+H,Edge);
        for (float DX : {0.f,W}) for (float DY : {0.f,H})
        {
            const float DirX=DX==0.f ? 1.f : -1.f, DirY=DY==0.f ? 1.f : -1.f;
            Line(PX+DX,PY+DY,PX+DX+DirX*12.f,PY+DY,Edge,2.f);
            Line(PX+DX,PY+DY,PX+DX,PY+DY+DirY*12.f,Edge,2.f);
        }
    }
    void Diamond(float CX, float CY, float Radius, const FLinearColor& Color) const
    { Line(CX,CY-Radius,CX+Radius,CY,Color); Line(CX+Radius,CY,CX,CY+Radius,Color);
      Line(CX,CY+Radius,CX-Radius,CY,Color); Line(CX-Radius,CY,CX,CY-Radius,Color); }
    void Circle(float CX, float CY, float Radius, const FLinearColor& Color, float Width=1.f) const
    {
        for (int32 I=0;I<40;++I)
        {
            const float A=I*2.f*PI/40.f,B=(I+1)*2.f*PI/40.f;
            Line(CX+FMath::Cos(A)*Radius,CY+FMath::Sin(A)*Radius,
                CX+FMath::Cos(B)*Radius,CY+FMath::Sin(B)*Radius,Color,Width);
        }
    }
    void Path(float CX, float CY, float Size, const FLinearColor& Color,
        std::initializer_list<FVector2D> Points, bool Closed=false, float Width=2.f) const
    {
        if (Points.size()<2) return;
        auto It=Points.begin(); FVector2D First=*It++, Previous=First;
        for (;It!=Points.end();++It)
        { Line(CX+Previous.X*Size,CY+Previous.Y*Size,CX+It->X*Size,CY+It->Y*Size,Color,Width); Previous=*It; }
        if (Closed) Line(CX+Previous.X*Size,CY+Previous.Y*Size,CX+First.X*Size,CY+First.Y*Size,Color,Width);
    }
    // Glyph IDs: abilities 0..5, equipment blade/guard/helmet/chest/boots 6..10.
    void Icon(int32 ID, float CX, float CY, float Size, const FLinearColor& Color) const
    {
        switch (ID)
        {
        case 0: case 6:
            Path(CX,CY,Size,Color,{{-.17f,.24f},{-.17f,-.23f},{0.f,-.47f},{.17f,-.23f},{.17f,.24f}},true);
            Line(CX-.30f*Size,CY+.24f*Size,CX+.30f*Size,CY+.24f*Size,Color,3.f);
            Line(CX,CY+.24f*Size,CX,CY+.49f*Size,Color,3.f);
            if (ID==0) Path(CX,CY,Size,Color,{{-.43f,-.38f},{-.53f,-.12f},{-.45f,.13f},{-.32f,.32f}});
            break;
        case 1:
            Path(CX,CY,Size,Color,{{-.45f,-.28f},{-.13f,0.f},{-.45f,.28f}});
            Path(CX,CY,Size,Color,{{-.12f,-.28f},{.22f,0.f},{-.12f,.28f}});
            Path(CX,CY,Size,Color,{{.21f,-.28f},{.52f,0.f},{.21f,.28f}}); break;
        case 2:
            Path(CX,CY,Size,Color,{{0.f,-.48f},{.39f,-.28f},{.30f,.20f},{0.f,.48f},{-.30f,.20f},{-.39f,-.28f}},true);
            Line(CX,CY-.26f*Size,CX,CY+.21f*Size,Color);
            Line(CX-.18f*Size,CY-.03f*Size,CX+.18f*Size,CY-.03f*Size,Color); break;
        case 3:
            Icon(6,CX,CY,Size*.82f,Color);
            Path(CX,CY,Size,Color,{{-.38f,.18f},{-.56f,-.05f},{-.34f,-.31f}});
            Path(CX,CY,Size,Color,{{.38f,.18f},{.56f,-.05f},{.34f,-.31f}});
            Diamond(CX,CY-.48f*Size,Size*.09f,Gold); break;
        case 4:
            Path(CX,CY,Size,Color,{{.18f,-.48f},{-.30f,.02f},{.04f,.02f},{-.16f,.47f},{.32f,-.10f},{.03f,-.10f}},true);
            Diamond(CX-.40f*Size,CY-.32f*Size,Size*.10f,Gold); break;
        case 5:
            Path(CX,CY,Size,Color,{{-.39f,-.42f},{.33f,.35f}});
            Path(CX,CY,Size,Color,{{.39f,-.42f},{-.33f,.35f}});
            Line(CX-.44f*Size,CY+.20f*Size,CX-.18f*Size,CY+.45f*Size,Color,3.f);
            Line(CX+.44f*Size,CY+.20f*Size,CX+.18f*Size,CY+.45f*Size,Color,3.f); break;
        case 7:
            Path(CX,CY,Size,Color,{{-.49f,.11f},{-.42f,-.12f},{-.12f,-.04f},{0.f,-.19f},{.12f,-.04f},{.42f,-.12f},{.49f,.11f},{.12f,.17f},{0.f,.33f},{-.12f,.17f}},true);
            Diamond(CX,CY+.02f*Size,Size*.09f,Gold); break;
        case 8:
            Path(CX,CY,Size,Color,{{-.36f,.38f},{-.40f,-.15f},{-.24f,-.39f},{.24f,-.39f},{.40f,-.15f},{.36f,.38f},{.13f,.45f},{.13f,.05f},{-.13f,.05f},{-.13f,.45f}},true);
            Line(CX-.28f*Size,CY-.08f*Size,CX+.28f*Size,CY-.08f*Size,Color,3.f); break;
        case 9:
            Path(CX,CY,Size,Color,{{-.19f,-.41f},{-.44f,-.26f},{-.49f,.01f},{-.30f,.06f},{-.24f,.42f},{.24f,.42f},{.30f,.06f},{.49f,.01f},{.44f,-.26f},{.19f,-.41f},{0.f,-.20f}},true);
            Line(CX,CY-.10f*Size,CX,CY+.29f*Size,Color);
            Path(CX,CY,Size,Color,{{-.22f,.16f},{0.f,.23f},{.22f,.16f}}); break;
        case 10:
            Path(CX,CY,Size,Color,{{-.36f,-.43f},{-.09f,-.43f},{-.09f,.20f},{.04f,.30f},{.04f,.44f},{-.40f,.44f}},true);
            Path(CX,CY,Size,Color,{{.09f,-.43f},{.36f,-.43f},{.36f,.20f},{.49f,.30f},{.49f,.44f},{.08f,.44f}},true); break;
        case 11:
            Path(CX,CY,Size,Color,{{-.04f,.22f},{-.04f,-.28f},{0.f,-.50f},{.04f,-.28f},{.04f,.22f}},true);
            Circle(CX+.05f*Size,CY+.25f*Size,.16f*Size,Color);
            Line(CX-.18f*Size,CY+.22f*Size,CX+.18f*Size,CY+.22f*Size,Color);
            Line(CX,CY+.22f*Size,CX,CY+.48f*Size,Color,3.f); break;
        case 12:
            Path(CX,CY,Size,Color,{{0.f,-.5f},{.15f,-.22f},{.18f,-.37f},{.4f,.0f},{.33f,.33f},{.10f,.48f},{-.2f,.4f},{-.4f,.15f},{-.30f,-.13f},{-.14f,.02f},{-.09f,-.25f}},true);
            Path(CX,CY,Size,Color,{{0.f,-.05f},{.14f,.24f},{0.f,.4f},{-.12f,.23f}},true); break;
        case 13:
            Path(CX,CY,Size,Color,{{0.f,-.05f},{-.18f,-.24f},{-.36f,-.2f},{-.41f,0.f},{0.f,.41f},{.41f,0.f},{.36f,-.2f},{.18f,-.24f}},true);
            Diamond(CX,CY-.42f*Size,Size*.09f,Gold); break;
        }
    }
    bool Hover(float PX,float PY,float W,float H) const
    { float MX=0.f,MY=0.f; auto* PC=HUD.GetOwningPlayerController(); return PC && PC->GetMousePosition(MX,MY) && Rect(PX,PY,W,H).Contains({MX,MY}); }
    void Button(FName ID,const FString& Caption,float PX,float PY,float W,float H,bool Enabled=true,bool Selected=false) const
    {
        const bool Hot=Hover(PX,PY,W,H);
        const FLinearColor FillColor=Selected ? Blue : Enabled ? (Hot ? PaleBlue : FLinearColor(.86f,.91f,.93f)) : FLinearColor(.87f,.89f,.86f);
        Frame(PX,PY,W,H,FillColor,Selected || Hot ? Gold : FLinearColor(.61f,.72f,.77f),false);
        Text(Caption,PX+W*.5f,PY+(H-17.f)*.5f,.95f,Selected ? Ivory : Enabled ? Ink : Muted,W-16.f,true);
        const FMistHUDRect R=Rect(PX,PY,W,H);
        HUD.AddHitBox({R.X,R.Y},{R.W,R.H},ID,true);
    }
};

FString HeroStatus(AMistworldPlayerController* PC,const UMistCombatHealth* HP)
{
    if (!HP) return PC->GetHeroDisplayName();
    if (!HP->IsAlive()) return FString::Printf(TEXT("Reviving in %.1fs"),PC->GetRespawnRemaining());
    if (HP->IsSpawnProtected()) return FString::Printf(TEXT("Spawn protection  %.1fs"),HP->GetSpawnProtectionRemaining());
    if (HP->IsStunned()) return TEXT("Stunned");
    if (HP->IsRooted()) return TEXT("Rooted");
    if (HP->IsSilenced()) return TEXT("Silenced");
    if (PC->IsCombatBusy()) return TEXT("Casting");
    if (PC->IsAnima()) return PC->GetHeroStatusSummary();
    if (PC->GetDefenseRemaining()>0.f) return FString::Printf(TEXT("Guard  %.1fs"),PC->GetDefenseRemaining());
    if (PC->GetWarcryRemaining()>0.f) return FString::Printf(TEXT("Abyss Warcry  %.1fs"),PC->GetWarcryRemaining());
    return TEXT("Greatsword  /  Heavy armor");
}
}

void AMistCombatHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (IsValid(InventoryPreview)) InventoryPreview->Destroy();
    InventoryPreview=nullptr;
    Super::EndPlay(EndPlayReason);
}

void AMistCombatHUD::DrawHUD()
{
    Super::DrawHUD();
    auto* PC=Cast<AMistworldPlayerController>(GetOwningPlayerController());
    if (!Canvas || !PC || !PC->GetPawn() || !GEngine)
    { if (IsValid(InventoryPreview)) InventoryPreview->SetPreviewActive(false); return; }
    UpdateInventoryPreview(PC);
    if (PC->IsInventoryMenuOpen()) { DrawInventoryMenu(PC); return; }
    DrawQuestCard(PC);
    DrawCombatDock(PC);
    if (PC->IsLoadoutMenuOpen()) DrawLoadoutMenu(PC);
}

void AMistCombatHUD::UpdateInventoryPreview(AMistworldPlayerController* PC)
{
    if (!PC->IsInventoryMenuOpen())
    { if (InventoryPreview) InventoryPreview->SetPreviewActive(false); return; }
    if (!IsValid(InventoryPreview))
    {
        FActorSpawnParameters Params; Params.Owner=this; Params.ObjectFlags=RF_Transient;
        Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        InventoryPreview=GetWorld()->SpawnActor<AMistInventoryPreview>(FVector(0,0,-50000),FRotator::ZeroRotator,Params);
        NextPreviewUpdateAt=0.0;
    }
    if (!InventoryPreview) return;
    const bool Opening=!InventoryPreview->IsPreviewActive();
    const double Now=GetWorld()->GetTimeSeconds();
    if (Opening || Now>=NextPreviewUpdateAt)
    { InventoryPreview->UpdateFromPawn(PC->GetPawn()); NextPreviewUpdateAt=Now+.20; }
    InventoryPreview->SetPreviewActive(true);
}

void AMistCombatHUD::DrawQuestCard(AMistworldPlayerController* PC)
{
    const float S=FMath::Min(1.f,(Canvas->SizeX-32.f)/560.f);
    if (S<=0.f) return;
    FPainter P(*this,{16.f,16.f,560.f*S,88.f*S},S);
    P.Frame(0,0,560,88,FLinearColor(Ivory.R,Ivory.G,Ivory.B,.92f));
    P.Diamond(22,22,6,Gold);
    P.Text(PC->GetCryptSession() ? TEXT("THE ELEMENTAL JAIL") : TEXT("THE TRAINING GROUNDS"),38,12,.92f,Blue,510);
    const FString Objective=PC->GetCryptSession() ? PC->GetCryptSession()->GetObjectiveText() : FString::Printf(TEXT("Master %s's original and %s abilities."),*PC->GetHeroDisplayName(),*PC->GetAscensionDisplayName());
    P.Text(Objective,18,34,.95f,Ink,524);
    FString Hint=PC->GetCryptSession() ? PC->GetCryptSession()->GetInteractionHint() : FString();
    if (Hint.IsEmpty()) Hint=TEXT("I  Inventory     L  Characters & skills     Hold a skill to aim, release to cast");
    P.Text(Hint,18,62,.80f,Muted,524);
}

void AMistCombatHUD::DrawCombatDock(AMistworldPlayerController* PC)
{
    const FMistHUDLayout Layout=FMistHUDLayout::ForViewport(Canvas->SizeX,Canvas->SizeY);
    FPainter P(*this,Layout.Dock,Layout.DockScale);
    const UMistCombatHealth* HP=PC->GetPawn()->FindComponentByClass<UMistCombatHealth>();
    P.Frame(0,0,860,130,FLinearColor(.035f,.085f,.145f,.96f));
    P.Line(18,3,842,3,FLinearColor(.85f,.77f,.48f,.48f));
    P.Text(PC->GetHeroDisplayName().ToUpper(),20,16,1.28f,Ivory,185,false,true);
    P.Text(HeroStatus(PC,HP),20,45,.81f,PaleBlue,185);
    const float Fraction=HP && HP->MaxHealth>0.f ? FMath::Clamp(HP->Health/HP->MaxHealth,0.f,1.f) : 0.f;
    P.Fill(20,75,185,8,Ink);
    P.Fill(20,75,185*Fraction,8,HealthColor);
    P.Text(FString::Printf(TEXT("%.0f  /  %.0f"),HP ? HP->Health : 0.f,HP ? HP->MaxHealth : 0.f),20,91,.85f,Ivory,185);
    P.Line(223,20,223,109,FLinearColor(.45f,.56f,.62f,.50f));
    const FKey Keys[]={EKeys::Q,EKeys::W,EKeys::E,EKeys::R,EKeys::F,EKeys::SpaceBar};
    const TCHAR* BaseNames[]={TEXT("Slash"),TEXT("Dash"),TEXT("Guard"),TEXT("Return"),TEXT("Flash"),TEXT("Strike")};
    const TCHAR* AbyssNames[]={TEXT("Quake"),TEXT("Abyss"),TEXT("Abyss"),TEXT("Warcry"),TEXT("Flash"),TEXT("Strike")};
    const TCHAR* AnimaNames[]={TEXT("Mark"),TEXT("Dashes"),TEXT("Riposte"),TEXT("Steal"),TEXT("Flash"),TEXT("Thrust")};
    const TCHAR* FlameNames[]={TEXT("Brand"),TEXT("Ember"),TEXT("Parry"),TEXT("Inferno"),TEXT("Flash"),TEXT("Thrust")};
    for (int32 I=0;I<6;++I)
    {
        const float X=242.f+I*101.f;
        const float Remaining=PC->GetCooldownRemaining(Keys[I]);
        const bool Recast=(I==3 && PC->GetUltimateRecastRemaining()>0.f)
            || (PC->IsAnima() && I==1 && PC->GetAbilityStatus(Keys[I]).Contains(TEXT("RECAST")));
        const bool Cooling=Remaining>0.f && !Recast;
        const bool Abyss=I<4 && PC->IsAbyssEquipped(Keys[I]);
        const FLinearColor Accent=Abyss ? (PC->IsAnima() ? FLinearColor(.77f,.31f,.17f) : FLinearColor(.46f,.38f,.67f)) : Blue;
        P.Frame(X,13,84,77,Cooling ? FLinearColor(.61f,.71f,.76f) : Ivory,Recast ? Gold : PaleBlue,false);
        P.Fill(X+1,14,82,3,Recast ? Gold : Accent);
        const int32 Glyph=PC->IsAnima() ? (I==0 || I==5 ? 11 : I==3 ? (Abyss ? 12 : 13) : I) : I;
        P.Icon(Glyph,X+42,46,47,Cooling ? FLinearColor(.30f,.41f,.47f) : Accent);
        if (Cooling)
        {
            const float CD=FMath::Max(.01f,PC->GetCooldownDisplayDuration(Keys[I]));
            const float Ratio=FMath::Clamp(Remaining/CD,0.f,1.f);
            P.Fill(X+1,89-72*Ratio,82,72*Ratio,FLinearColor(.025f,.07f,.11f,.61f));
            const FString Number=Remaining>=1.f ? FString::Printf(TEXT("%.0f"),FMath::CeilToFloat(Remaining)) : FString::Printf(TEXT("%.1f"),Remaining);
            P.Frame(X+18,29,48,32,FLinearColor(Ink.R,Ink.G,Ink.B,.92f),PaleBlue,false);
            P.Text(Number,X+42,32,1.68f,Ivory,72,true,true);
        }
        else if (Recast)
        { P.Text(TEXT("RECAST"),X+42,69,.72f,Blue,76,true); }
        else
        {
            FString State=I<5 ? PC->GetAbilityStatus(Keys[I]) : TEXT("READY");
            P.Text(State,X+42,69,.70f,Blue,76,true);
        }
        P.Frame(X+7,85,30,22,Ink,Gold,false);
        P.Text(I==5 ? TEXT("SPC") : Keys[I].ToString(),X+22,89,.77f,Ivory,26,true);
        const TCHAR* SkillName=PC->IsAnima() ? (Abyss ? FlameNames[I] : AnimaNames[I]) : (Abyss ? AbyssNames[I] : BaseNames[I]);
        P.Text(SkillName,X+43,91,.79f,PaleBlue,41);
    }
}

void AMistCombatHUD::DrawInventoryMenu(AMistworldPlayerController* PC)
{
    const auto* Progress=PC->GetProgression(); if (!Progress) return;
    const FMistHUDLayout Layout=FMistHUDLayout::ForViewport(Canvas->SizeX,Canvas->SizeY);
    FPainter P(*this,Layout.Inventory,Layout.InventoryScale);
    DrawRect(FLinearColor(.018f,.045f,.085f,.69f),0,0,Canvas->SizeX,Canvas->SizeY);
    P.Frame(0,0,1080,640);
    P.Fill(1,1,1078,91,FLinearColor(.85f,.91f,.93f));
    P.Text(PC->GetHeroDisplayName().ToUpper(),28,20,2.05f,Ink,410,false,true);
    P.Text(PC->IsAnima() ? TEXT("EQUIPMENT  /  RAPIER, DAGGER & LIGHT ARMOR") : TEXT("EQUIPMENT  /  GREATSWORD & HEAVY ARMOR"),30,61,.92f,Blue,580);
    P.Button(TEXT("Mist.Close"),TEXT("Close  I"),963,27,88,35);
    P.Line(27,92,1053,92,Gold);
    P.Text(TEXT("WEAPON COMPONENTS"),30,112,.88f,Muted,282);
    P.Text(PC->IsAnima() ? TEXT("LIGHT ARMOR") : TEXT("HEAVY ARMOR"),30,291,.88f,Muted,282);
    const auto Slots=UMistProgressionComponent::GetEquipmentSlotsForHero(PC->GetActiveHero());
    if (!Slots.Contains(SelectedEquipmentSlot)) SelectedEquipmentSlot=Slots[0];
    const float Rows[]={138.f,209.f,319.f,390.f,461.f};
    for (int32 I=0;I<Slots.Num();++I)
    {
        const bool Selected=SelectedEquipmentSlot==Slots[I];
        const bool Equipped=!Progress->GetEquippedItemForSlot(Slots[I]).IsNone();
        const bool Owned=!Progress->GetOwnedItemForSlot(Slots[I]).IsNone();
        const bool Hot=P.Hover(28,Rows[I],284,61);
        P.Frame(28,Rows[I],284,61,Selected ? Blue : Hot ? PaleBlue : FLinearColor(.90f,.93f,.92f),Selected ? Gold : FLinearColor(.66f,.76f,.80f),false);
        P.Icon(PC->IsAnima() && I<2 ? (I==0 ? 11 : 5) : 6+I,59,Rows[I]+30,37,Selected ? Ivory : Blue);
        P.Text(UMistProgressionComponent::GetEquipmentSlotDisplayName(Slots[I]),92,Rows[I]+11,.94f,Selected ? Ivory : Ink,209);
        P.Text(Equipped ? TEXT("Equipped") : Owned ? TEXT("Item available") : TEXT("Empty"),92,Rows[I]+35,.80f,Selected ? PaleBlue : Muted,198);
        const FMistHUDRect R=P.Rect(28,Rows[I],284,61);
        AddHitBox({R.X,R.Y},{R.W,R.H},FName(*FString::Printf(TEXT("Mist.Slot.%d"),I)),true);
    }
    P.Text(Progress->IsRewardEquipped() ? TEXT("1 / 5 SLOTS EQUIPPED") : TEXT("0 / 5 SLOTS EQUIPPED"),30,548,.83f,Muted,282);
    // The render target contains the actual equipped skeletal mesh and sword.
    P.Frame(334,111,402,439,FLinearColor(.06f,.11f,.18f),Gold,false);
    UTextureRenderTarget2D* Target=InventoryPreview ? InventoryPreview->GetRenderTarget() : nullptr;
    if (Target && Target->GetResource() && InventoryPreview->HasCharacterModel())
    {
        const float Aspect=Target->SizeY>0 ? static_cast<float>(Target->SizeX)/Target->SizeY : .667f;
        const float H=435.f, W=FMath::Min(398.f,H*Aspect);
        DrawTexture(Target,P.X+(535.f-W*.5f)*P.S,P.Y+113.f*P.S,W*P.S,H*P.S,
            0.f,0.f,1.f,1.f,FLinearColor::White,BLEND_Opaque);
    }
    else P.Text(TEXT("Character preview loading"),535,310,.92f,PaleBlue,350,true);
    P.Diamond(346,123,6,Gold); P.Diamond(724,123,6,Gold);
    P.Button(TEXT("Mist.Preview.Left"),TEXT("<"),391,564,58,30);
    P.Button(TEXT("Mist.Preview.Reset"),TEXT("Reset view"),461,564,148,30);
    P.Button(TEXT("Mist.Preview.Right"),TEXT(">"),621,564,58,30);
    const auto* HP=PC->GetPawn()->FindComponentByClass<UMistCombatHealth>();
    P.Text(HeroStatus(PC,HP),535,603,.81f,Muted,390,true);
    P.Line(758,112,758,594,FLinearColor(.70f,.78f,.80f));
    const bool Blade=SelectedEquipmentSlot==EMistEquipmentSlot::GreatswordBlade;
    const bool Owned=!Progress->GetOwnedItemForSlot(SelectedEquipmentSlot).IsNone();
    const bool Equipped=!Progress->GetEquippedItemForSlot(SelectedEquipmentSlot).IsNone();
    P.Text(TEXT("SELECTED SLOT"),780,115,.80f,Muted,270);
    P.Text(UMistProgressionComponent::GetEquipmentSlotDisplayName(SelectedEquipmentSlot),780,141,1.25f,Ink,273,false,true);
    P.Frame(832,193,170,132,FLinearColor(.85f,.91f,.93f),FLinearColor(.65f,.75f,.79f),false);
    const int32 SlotIndex=Slots.IndexOfByKey(SelectedEquipmentSlot);
    P.Icon(PC->IsAnima() && SlotIndex<2 ? (SlotIndex==0 ? 11 : 5) : 6+SlotIndex,917,259,101,Owned ? Blue : Muted);
    if (Owned)
    {
        P.Text(TEXT("Necrotic Greatsword"),780,345,1.12f,Ink,273,false,true);
        P.Text(TEXT("BLADE COMPONENT"),780,374,.80f,Blue,273);
        P.Text(TEXT("+5%"),780,413,1.65f,Blue,100,false,true);
        P.Text(TEXT("ABILITY DAMAGE"),873,427,.81f,Muted,180);
        P.Text(TEXT("Basic attacks retain their base damage."),780,459,.78f,Muted,271);
    }
    else
    {
        P.Text(TEXT("Empty slot"),780,345,1.20f,Ink,273,false,true);
        P.Wrapped(PC->IsAnima() ? TEXT("Anima's prototype equipment slots are empty. Alphonse's earned greatsword stays with him.") : Blade ? TEXT("Defeat the jail golem and claim its chest to earn a blade.")
            : TEXT("Equipment for this slot will appear here when you earn it."),780,381,271,.96f,Muted,3);
    }
    const FString Lock=PC->GetLoadoutLockReason();
    P.Button(Equipped ? TEXT("Mist.Unequip") : TEXT("Mist.Equip"),Owned ? (Equipped ? TEXT("Unequip blade") : TEXT("Equip blade")) : TEXT("No item owned"),780,499,272,42,
        Blade && Owned && Lock.IsEmpty(),Equipped);
    P.Wrapped(Lock.IsEmpty() ? TEXT("At a checkpoint. Equipment changes preserve cooldowns.") : Lock,780,557,272,.81f,Muted,2);
    P.Line(28,592,307,592,Gold);
    P.Text(Progress->GetSaveStatus(),28,607,.76f,Muted,282);
}

void AMistCombatHUD::DrawLoadoutMenu(AMistworldPlayerController* PC)
{
    const float S=FMath::Min(1.f,FMath::Min((Canvas->SizeX-32.f)/560.f,(Canvas->SizeY-32.f)/510.f));
    FPainter P(*this,{(Canvas->SizeX-560*S)*.5f,(Canvas->SizeY-510*S)*.5f,560*S,510*S},S);
    DrawRect(FLinearColor(.018f,.045f,.085f,.56f),0,0,Canvas->SizeX,Canvas->SizeY);
    P.Frame(0,0,560,510);
    P.Text(TEXT("CHARACTERS / ABILITIES"),24,21,1.45f,Ink,510,false,true);
    P.Line(24,56,536,56,Gold);
    const FString Lock=PC->GetLoadoutLockReason(); const bool Ready=Lock.IsEmpty();
    const FString SwitchLock=PC->GetCharacterSwitchLockReason();
    P.Text(SwitchLock.IsEmpty() ? TEXT("Switch at a safe checkpoint. Each hero keeps their loadout.") : SwitchLock,24,69,.9f,SwitchLock.IsEmpty() ? Blue : Muted,512);
    P.Button(TEXT("Mist.Hero.Alphonse"),TEXT("Alphonse / Brawler"),24,100,251,38,SwitchLock.IsEmpty(),!PC->IsAnima());
    P.Button(TEXT("Mist.Hero.Anima"),TEXT("Anima / Duelist"),285,100,251,38,SwitchLock.IsEmpty(),PC->IsAnima());
    if (!PC->IsAbyssUnlocked()) P.Button(TEXT("Mist.Unlock"),FString::Printf(TEXT("Unlock %s / test access"),*PC->GetAscensionDisplayName()),24,153,512,36,Ready);
    else P.Text(PC->GetAscensionDisplayName().ToUpper()+TEXT(" UNLOCKED / MIX ANY FOUR SKILLS"),24,163,.95f,Blue,512);
    const FKey Keys[]={EKeys::Q,EKeys::W,EKeys::E,EKeys::R};
    const TCHAR* Original[]={TEXT("Sword Slash"),TEXT("Spinning Dash"),TEXT("Guard"),TEXT("Returning Sword")};
    const TCHAR* Abyss[]={TEXT("Greatsword Quake"),TEXT("Abyss Dash"),TEXT("Abyss Guard"),TEXT("Abyss Warcry")};
    const TCHAR* Anima[]={TEXT("Mark"),TEXT("Three Dashes"),TEXT("Riposte"),TEXT("Anima Steal")};
    const TCHAR* Flame[]={TEXT("Brand"),TEXT("Ember Dashes"),TEXT("Flame Parry"),TEXT("Flamestealer Aura")};
    for (int32 I=0;I<4;++I)
    {
        const float Y=211.f+I*48.f; P.Text(Keys[I].ToString(),24,Y+10,1.05f,Blue,25);
        P.Button(FName(*FString::Printf(TEXT("Mist.Original.%d"),I)),PC->IsAnima() ? Anima[I] : Original[I],57,Y,232,38,Ready,!PC->IsAbyssEquipped(Keys[I]));
        P.Button(FName(*FString::Printf(TEXT("Mist.Abyss.%d"),I)),PC->IsAnima() ? Flame[I] : Abyss[I],299,Y,237,38,Ready && PC->IsAbyssUnlocked(),PC->IsAbyssEquipped(Keys[I]));
    }
    P.Text(Ready ? TEXT("Safe checkpoint / cooldowns and health are preserved") : Lock,24,413,.88f,Muted,512);
    P.Text(TEXT("Original skills on the left; ascension skills on the right."),24,440,.81f,Muted,512);
    if (PC->GetProgression()) P.Text(PC->GetProgression()->GetSaveStatus(),24,458,.68f,Muted,512);
    P.Button(TEXT("Mist.Close"),TEXT("Close  L"),24,473,512,30);
}

void AMistCombatHUD::NotifyHitBoxClick(const FName BoxName)
{
    Super::NotifyHitBoxClick(BoxName);
    auto* PC=Cast<AMistworldPlayerController>(GetOwningPlayerController());
    if (!PC || !PC->IsCombatMenuOpen() || BoxName==TEXT("Mist.Close")) return;
    if (PC->IsInventoryMenuOpen())
    {
        const auto Slots=UMistProgressionComponent::GetEquipmentSlotsForHero(PC->GetActiveHero());
        for (int32 I=0;I<Slots.Num();++I)
            if (BoxName==FName(*FString::Printf(TEXT("Mist.Slot.%d"),I))) SelectedEquipmentSlot=Slots[I];
        if (SelectedEquipmentSlot==EMistEquipmentSlot::GreatswordBlade)
        {
            if (BoxName==TEXT("Mist.Equip")) PC->TryEquipCryptReward(true);
            if (BoxName==TEXT("Mist.Unequip")) PC->TryEquipCryptReward(false);
        }
        if (InventoryPreview)
        {
            if (BoxName==TEXT("Mist.Preview.Left")) InventoryPreview->Rotate(-30.f);
            if (BoxName==TEXT("Mist.Preview.Right")) InventoryPreview->Rotate(30.f);
            if (BoxName==TEXT("Mist.Preview.Reset")) InventoryPreview->ResetRotation();
        }
        return;
    }
    if (BoxName==TEXT("Mist.Hero.Alphonse")) { PC->TrySwitchHero(EMistHeroId::Alphonse); return; }
    if (BoxName==TEXT("Mist.Hero.Anima")) { PC->TrySwitchHero(EMistHeroId::Anima); return; }
    if (BoxName==TEXT("Mist.Unlock")) { PC->SetAbyssUnlocked(true); return; }
    const FKey Keys[]={EKeys::Q,EKeys::W,EKeys::E,EKeys::R};
    for (int32 I=0;I<4;++I)
    {
        if (BoxName==FName(*FString::Printf(TEXT("Mist.Original.%d"),I))) PC->TryEquipAbility(Keys[I],false);
        if (BoxName==FName(*FString::Printf(TEXT("Mist.Abyss.%d"),I))) PC->TryEquipAbility(Keys[I],true);
    }
}

void AMistCombatHUD::NotifyHitBoxRelease(const FName BoxName)
{
    Super::NotifyHitBoxRelease(BoxName);
    auto* PC=Cast<AMistworldPlayerController>(GetOwningPlayerController());
    // Keep the menu up throughout the press so it cannot also issue a move.
    if (PC && BoxName==TEXT("Mist.Close"))
    { if (PC->IsInventoryMenuOpen()) PC->ToggleInventoryMenu(); else if (PC->IsLoadoutMenuOpen()) PC->ToggleLoadoutMenu(); }
}
