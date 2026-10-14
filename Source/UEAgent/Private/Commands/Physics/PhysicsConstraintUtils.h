#pragma once

#include "CoreMinimal.h"
#include "Chaos/ChaosEngineInterface.h"         // ELinearConstraintMotion / EAngularConstraintMotion
#include "Commands/Common/McpCommonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PhysicsEngine/ConstraintDrives.h"     // EAngularDriveMode
#include "PhysicsEngine/ConstraintInstance.h"

// Constraint helpers shared by the physics-asset constraint commands and the physics-constraint-actor commands: the
// motion and drive-mode names, and the drive, breakable and projection settings both kinds of constraint take.
namespace PhysicsConstraintUtils
{
    inline const TCHAR* LinearMotionName(ELinearConstraintMotion Motion)
    {
        switch (Motion)
        {
            case LCM_Free:    return TEXT("Free");
            case LCM_Limited: return TEXT("Limited");
            case LCM_Locked:  return TEXT("Locked");
            default:          return TEXT("Unknown");
        }
    }

    inline const TCHAR* AngularMotionName(EAngularConstraintMotion Motion)
    {
        switch (Motion)
        {
            case ACM_Free:    return TEXT("Free");
            case ACM_Limited: return TEXT("Limited");
            case ACM_Locked:  return TEXT("Locked");
            default:          return TEXT("Unknown");
        }
    }

    inline const TCHAR* AngularDriveModeName(EAngularDriveMode::Type Mode)
    {
        return (Mode == EAngularDriveMode::TwistAndSwing) ? TEXT("TwistAndSwing") : TEXT("SLERP");
    }

    /** SLERP or TwistAndSwing, in any case; false, leaving Out as it is, for any other value. */
    inline bool ParseAngularDriveMode(const FString& In, EAngularDriveMode::Type& Out)
    {
        if (In.Equals(TEXT("SLERP"), ESearchCase::IgnoreCase))
        {
            Out = EAngularDriveMode::SLERP;
            return true;
        }
        if (In.Equals(TEXT("TwistAndSwing"), ESearchCase::IgnoreCase))
        {
            Out = EAngularDriveMode::TwistAndSwing;
            return true;
        }
        return false;
    }

    /**
     * The first three numbers of Values as an FVector (X, Y, Z) or an FRotator (Pitch, Yaw, Roll); false, leaving Out
     * as it is, when Values is null or holds fewer than three.
     */
    template <typename TripleType>
    inline bool ReadNumberTriple(const TArray<TSharedPtr<FJsonValue>>* Values, TripleType& Out)
    {
        if (Values == nullptr || Values->Num() < 3)
        {
            return false;
        }
        Out = TripleType((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
        return true;
    }

    /** One optional value of a command: Value counts only when bGiven. */
    template <typename ValueType>
    struct TSetting
    {
        ValueType Value = ValueType();
        bool bGiven = false;

        /** Value converted to the type of Current when given, else Current. */
        template <typename CurrentType>
        CurrentType Or(CurrentType Current) const
        {
            return bGiven ? static_cast<CurrentType>(Value) : Current;
        }
    };

    /** The drive, breakable and projection settings a command asks for; one not given keeps the constraint's value. */
    struct FDriveSettings
    {
        TSetting<EAngularDriveMode::Type> AngularDriveMode;
        TSetting<bool> AngularOrientationDrive;
        TSetting<bool> AngularVelocityDrive;
        TSetting<double> AngularDriveSpring;
        TSetting<double> AngularDriveDamping;
        TSetting<double> AngularDriveForceLimit;
        TSetting<FRotator> AngularOrientationTarget;
        TSetting<bool> LinearPositionDrive;
        TSetting<bool> LinearVelocityDrive;
        TSetting<double> LinearDriveSpring;
        TSetting<double> LinearDriveDamping;
        TSetting<double> LinearDriveForceLimit;
        TSetting<FVector> LinearPositionTarget;
        TSetting<bool> LinearBreakable;
        TSetting<double> LinearBreakThreshold;
        TSetting<bool> AngularBreakable;
        TSetting<double> AngularBreakThreshold;
        TSetting<bool> ProjectionEnabled;
        TSetting<double> ProjectionLinearAlpha;
        TSetting<double> ProjectionAngularAlpha;
        TSetting<double> ProjectionLinearTolerance;
        TSetting<double> ProjectionAngularTolerance;
        TSetting<bool> ParentDominates;
    };

    /**
     * Writes the given settings into CI, one setter each, in this order: angular drive mode, orientation and velocity
     * drives (SLERP or twist-and-swing by the resulting mode), angular drive spring/damping/force limit, orientation
     * target, the same four for the linear drive, linear and angular breakable, projection, parent dominance. A setter
     * that takes several values keeps the current value of those not given. Returns how many setters ran.
     */
    inline int32 ApplyDriveSettings(FConstraintInstance& CI, const FDriveSettings& Settings)
    {
        int32 Applied = 0;
        const EAngularDriveMode::Type Mode = Settings.AngularDriveMode.Or(CI.GetAngularDriveMode());
        if (Settings.AngularDriveMode.bGiven)
        {
            CI.SetAngularDriveMode(Mode);
            ++Applied;
        }
        if (Settings.AngularOrientationDrive.bGiven)
        {
            const bool bEnable = Settings.AngularOrientationDrive.Value;
            if (Mode == EAngularDriveMode::SLERP)
            {
                CI.SetOrientationDriveSLERP(bEnable);
            }
            else
            {
                CI.SetOrientationDriveTwistAndSwing(bEnable, bEnable);
            }
            ++Applied;
        }
        if (Settings.AngularVelocityDrive.bGiven)
        {
            const bool bEnable = Settings.AngularVelocityDrive.Value;
            if (Mode == EAngularDriveMode::SLERP)
            {
                CI.SetAngularVelocityDriveSLERP(bEnable);
            }
            else
            {
                CI.SetAngularVelocityDriveTwistAndSwing(bEnable, bEnable);
            }
            ++Applied;
        }
        if (Settings.AngularDriveSpring.bGiven || Settings.AngularDriveDamping.bGiven || Settings.AngularDriveForceLimit.bGiven)
        {
            float Spring, Damping, Force;
            CI.GetAngularDriveParams(Spring, Damping, Force);
            CI.SetAngularDriveParams(Settings.AngularDriveSpring.Or(Spring), Settings.AngularDriveDamping.Or(Damping),
                Settings.AngularDriveForceLimit.Or(Force));
            ++Applied;
        }
        if (Settings.AngularOrientationTarget.bGiven)
        {
            CI.SetAngularOrientationTarget(FQuat(Settings.AngularOrientationTarget.Value));
            ++Applied;
        }
        if (Settings.LinearPositionDrive.bGiven)
        {
            const bool bEnable = Settings.LinearPositionDrive.Value;
            CI.SetLinearPositionDrive(bEnable, bEnable, bEnable);
            ++Applied;
        }
        if (Settings.LinearVelocityDrive.bGiven)
        {
            const bool bEnable = Settings.LinearVelocityDrive.Value;
            CI.SetLinearVelocityDrive(bEnable, bEnable, bEnable);
            ++Applied;
        }
        if (Settings.LinearDriveSpring.bGiven || Settings.LinearDriveDamping.bGiven || Settings.LinearDriveForceLimit.bGiven)
        {
            float Spring, Damping, Force;
            CI.GetLinearDriveParams(Spring, Damping, Force);
            CI.SetLinearDriveParams(Settings.LinearDriveSpring.Or(Spring), Settings.LinearDriveDamping.Or(Damping),
                Settings.LinearDriveForceLimit.Or(Force));
            ++Applied;
        }
        if (Settings.LinearPositionTarget.bGiven)
        {
            CI.SetLinearPositionTarget(Settings.LinearPositionTarget.Value);
            ++Applied;
        }
        if (Settings.LinearBreakable.bGiven || Settings.LinearBreakThreshold.bGiven)
        {
            CI.SetLinearBreakable(Settings.LinearBreakable.Or(CI.IsLinearBreakable()),
                Settings.LinearBreakThreshold.Or(CI.GetLinearBreakThreshold()));
            ++Applied;
        }
        if (Settings.AngularBreakable.bGiven || Settings.AngularBreakThreshold.bGiven)
        {
            CI.SetAngularBreakable(Settings.AngularBreakable.Or(CI.IsAngularBreakable()),
                Settings.AngularBreakThreshold.Or(CI.GetAngularBreakThreshold()));
            ++Applied;
        }
        if (Settings.ProjectionEnabled.bGiven || Settings.ProjectionLinearAlpha.bGiven || Settings.ProjectionAngularAlpha.bGiven
            || Settings.ProjectionLinearTolerance.bGiven || Settings.ProjectionAngularTolerance.bGiven)
        {
            float LinAlpha, AngAlpha, LinTol, AngTol;
            CI.GetProjectionParams(LinAlpha, AngAlpha, LinTol, AngTol);
            CI.SetProjectionParams(Settings.ProjectionEnabled.Or(CI.IsProjectionEnabled()),
                Settings.ProjectionLinearAlpha.Or(LinAlpha), Settings.ProjectionAngularAlpha.Or(AngAlpha),
                Settings.ProjectionLinearTolerance.Or(LinTol), Settings.ProjectionAngularTolerance.Or(AngTol));
            ++Applied;
        }
        if (Settings.ParentDominates.bGiven)
        {
            CI.SetParentDominates(Settings.ParentDominates.Value);
            ++Applied;
        }
        return Applied;
    }

    /** angular_drive_spring, angular_drive_damping, angular_drive_force_limit and angular_orientation_target. */
    inline void AddAngularDriveFields(const TSharedPtr<FJsonObject>& Out, FConstraintInstance& CI)
    {
        float Spring, Damping, Force;
        CI.GetAngularDriveParams(Spring, Damping, Force);
        Out->SetNumberField(TEXT("angular_drive_spring"), Spring);
        Out->SetNumberField(TEXT("angular_drive_damping"), Damping);
        Out->SetNumberField(TEXT("angular_drive_force_limit"), Force);
        Out->SetArrayField(TEXT("angular_orientation_target"), mcp_common::RotatorToJsonArray(CI.GetAngularOrientationTarget()));
    }

    /** linear_drive_spring, linear_drive_damping, linear_drive_force_limit and linear_position_target. */
    inline void AddLinearDriveFields(const TSharedPtr<FJsonObject>& Out, FConstraintInstance& CI)
    {
        float Spring, Damping, Force;
        CI.GetLinearDriveParams(Spring, Damping, Force);
        Out->SetNumberField(TEXT("linear_drive_spring"), Spring);
        Out->SetNumberField(TEXT("linear_drive_damping"), Damping);
        Out->SetNumberField(TEXT("linear_drive_force_limit"), Force);
        Out->SetArrayField(TEXT("linear_position_target"), mcp_common::VectorToJsonArray(CI.GetLinearPositionTarget()));
    }

    /** projection_linear_alpha, projection_angular_alpha, projection_linear_tolerance and projection_angular_tolerance. */
    inline void AddProjectionFields(const TSharedPtr<FJsonObject>& Out, const FConstraintInstance& CI)
    {
        float LinAlpha, AngAlpha, LinTol, AngTol;
        CI.GetProjectionParams(LinAlpha, AngAlpha, LinTol, AngTol);
        Out->SetNumberField(TEXT("projection_linear_alpha"), LinAlpha);
        Out->SetNumberField(TEXT("projection_angular_alpha"), AngAlpha);
        Out->SetNumberField(TEXT("projection_linear_tolerance"), LinTol);
        Out->SetNumberField(TEXT("projection_angular_tolerance"), AngTol);
    }
}
