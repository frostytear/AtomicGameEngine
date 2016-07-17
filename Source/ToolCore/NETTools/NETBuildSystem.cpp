//
// Copyright (c) 2014-2016 THUNDERBEAST GAMES LLC
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include <Poco/Exception.h>
#include <Poco/Environment.h>


#include <Atomic/IO/Log.h>
#include <Atomic/IO/FileSystem.h>

#include "../ToolEvents.h"
#include "../ToolSystem.h"
#include "../ToolEnvironment.h"
#include "../Subprocess/SubprocessSystem.h"
#include "../Project/Project.h"

#include "NETProjectGen.h"
#include "NETBuildSystem.h"

namespace ToolCore
{

    NETBuild::NETBuild(Context* context, const String& solutionPath, const String& platform, const String& configuration) :
        Object(context),
        solutionPath_(solutionPath),
        platform_(platform),
        configuration_(configuration),
        status_(NETBUILD_PENDING)
    {

    }

    NETBuildSystem::NETBuildSystem(Context* context) :
        Object(context)
    {
        SubscribeToEvent(E_TOOLUPDATE, HANDLER(NETBuildSystem, HandleToolUpdate));
        SubscribeToEvent(E_NETBUILDATOMICPROJECT, HANDLER(NETBuildSystem, HandleBuildAtomicProject));
    }

    NETBuildSystem::~NETBuildSystem()
    {

    }

    void NETBuildSystem::HandleSubprocessOutput(StringHash eventType, VariantMap& eventData)
    {
        if (curBuild_.Null())
        {
            LOGERRORF("NETBuildSystem::HandleSubprocessOutput - output received without current build");
            return;
        }

        const String& text = eventData[SubprocessOutput::P_TEXT].GetString();

        // LOGINFOF(text.CString());

        curBuild_->output_ += text;

    }

    void NETBuildSystem::HandleCompileProcessComplete(StringHash eventType, VariantMap& eventData)
    {
        UnsubscribeFromEvent(E_SUBPROCESSCOMPLETE);
        UnsubscribeFromEvent(E_SUBPROCESSOUTPUT);

        if (curBuild_.Null())
        {
            LOGERROR("NETBuildSystem::HandleCompileProcessComplete - called with no current build");
            return;
        }
        curBuild_->status_ = NETBUILD_COMPLETE;

        int code = eventData[SubprocessComplete::P_RETCODE].GetInt();

        using namespace NETBuildResult;
        VariantMap buildEventData;

        buildEventData[P_BUILD] = curBuild_;

        if (!code)
        {
            buildEventData[P_SUCCESS] = true;
        }
        else
        {
            buildEventData[P_SUCCESS] = false;
            curBuild_->output_ += ToString("\nCompilation Command: %s", curBuild_->allArgs_.CString());
            buildEventData[P_ERRORTEXT] = curBuild_->output_;
        }

        curBuild_->SendEvent(E_NETBUILDRESULT, buildEventData);

        curBuild_ = nullptr;
    }

    void NETBuildSystem::CurrentBuildError(String errorText)
    {
        if (curBuild_.Null())
        {
            LOGERRORF("NETBuildSystem::CurrentBuildError - Error %s with no current build", errorText.CString());
            return;
        }

        using namespace NETBuildResult;
        VariantMap buildEventData;

        buildEventData[P_BUILD] = curBuild_;
        buildEventData[P_SUCCESS] = false;
        buildEventData[P_ERRORTEXT] = errorText;
        curBuild_->SendEvent(E_NETBUILDRESULT, buildEventData);
        curBuild_ = nullptr;

    }

    void NETBuildSystem::HandleToolUpdate(StringHash eventType, VariantMap& eventData)
    {
        if (curBuild_.Null() && !builds_.Size())
            return;

        if (curBuild_.Null())
        {
            // kick off a new build

            curBuild_ = builds_.Front();
            builds_.PopFront();


            FileSystem* fileSystem = GetSubsystem<FileSystem>();

            // Ensure solution still exists
            if (!fileSystem->FileExists(curBuild_->solutionPath_))
            {
                CurrentBuildError(ToString("Solution does not exist(%s)", curBuild_->solutionPath_.CString()));
                return;
            }

            String solutionPath = curBuild_->solutionPath_;

            String ext = GetExtension(solutionPath);

            bool requiresNuGet = true;

            if (ext == ".atomic")
            {
                if (curBuild_->project_.Null() || curBuild_->project_.Expired())
                {
                    CurrentBuildError(ToString("Error loading project (%s), project expired", solutionPath.CString()));
                }

                Project* project = curBuild_->project_;

                SharedPtr<NETProjectGen> gen(new NETProjectGen(context_));

                gen->SetScriptPlatform(curBuild_->platform_);

                if (!gen->LoadProject(project))
                {
                    CurrentBuildError(ToString("Error loading project (%s)", solutionPath.CString()));
                    return;
                }

                if (!gen->Generate())
                {
                    CurrentBuildError(ToString("Error generating project (%s)", solutionPath.CString()));
                    return;
                }

                solutionPath = gen->GetSolution()->GetOutputFilename();
                requiresNuGet = gen->GetRequiresNuGet();


                if (!fileSystem->FileExists(solutionPath))
                {
                    CurrentBuildError(ToString("Generated solution does not exist (%s : %s)", curBuild_->solutionPath_.CString(), solutionPath.CString()));
                    return;
                }

            }
            else if (ext == ".json")
            {
                SharedPtr<NETProjectGen> gen(new NETProjectGen(context_));

                gen->SetScriptPlatform(curBuild_->platform_);

                if (!gen->LoadProject(solutionPath))
                {
                    CurrentBuildError(ToString("Error loading project (%s)", solutionPath.CString()));
                    return;
                }

                if (!gen->Generate())
                {
                    CurrentBuildError(ToString("Error generating project (%s)", solutionPath.CString()));
                    return;
                }

                solutionPath = gen->GetSolution()->GetOutputFilename();
                requiresNuGet = gen->GetRequiresNuGet();

                if (!fileSystem->FileExists(solutionPath))
                {
                    CurrentBuildError(ToString("Generated solution does not exist (%s : %s)", curBuild_->solutionPath_.CString(), solutionPath.CString()));
                    return;
                }

            }

            ToolEnvironment* tenv = GetSubsystem<ToolEnvironment>();
            const String& nugetBinary = tenv->GetAtomicNETNuGetBinary();

            if (!fileSystem->FileExists(nugetBinary))
            {
                CurrentBuildError(ToString("NuGet binary is missing (%s)", nugetBinary.CString()));
                return;
            }

            String cmdToolsPath = Poco::Environment::get("VS140COMNTOOLS").c_str();

            if (!cmdToolsPath.Length())
            {
                CurrentBuildError("VS140COMNTOOLS environment variable not found, cannot proceed");
                return;
            }

            String vcvars64 = ToString("%s..\\..\\VC\\bin\\amd64\\vcvars64.bat", cmdToolsPath.CString());
            
            const String configuration = curBuild_->configuration_;

            String cmd = "cmd";
            Vector<String> args;
            args.Push("/A");
            args.Push("/C");

            // vcvars bat
            String compile = ToString("\"\"%s\" ", vcvars64.CString());

            if (requiresNuGet)
            {
                compile += ToString("&& \"%s\" restore \"%s\" ", nugetBinary.CString(), solutionPath.CString());
            }

            compile += ToString("&& msbuild \"%s\" /p:Configuration=%s /p:Platform=\"Any CPU\"\"", solutionPath.CString(), configuration.CString());

            args.Push(compile);

            curBuild_->allArgs_.Join(args, " ");

            SubprocessSystem* subs = GetSubsystem<SubprocessSystem>();
            Subprocess* subprocess = nullptr;

            try
            {
                subprocess = subs->Launch(cmd, args, "C:\\");
            }
            catch (Poco::SystemException)
            {
                subprocess = nullptr;
            }

            if (!subprocess)
            {
                CurrentBuildError(ToString("NETCompile::Compile - Unable to launch MSBuild subprocess\n%s", curBuild_->allArgs_.CString()));
                return;
            }

            SubscribeToEvent(subprocess, E_SUBPROCESSCOMPLETE, HANDLER(NETBuildSystem, HandleCompileProcessComplete));
            SubscribeToEvent(subprocess, E_SUBPROCESSOUTPUT, HANDLER(NETBuildSystem, HandleSubprocessOutput));

            curBuild_->status_ = NETBUILD_BUILDING;

        }

    }


    NETBuild* NETBuildSystem::GetBuild(const String& solutionPath, const String& platform,  const String& configuration)
    {
        List<SharedPtr<NETBuild>>::ConstIterator itr = builds_.Begin();

        while (itr != builds_.End())
        {
            NETBuild* build = *itr;

            if (build->solutionPath_ == solutionPath && build->platform_ == platform && build->configuration_ == configuration)
                return build;

            itr++;
        }

        return nullptr;

    }

    void NETBuildSystem::HandleBuildAtomicProject(StringHash eventType, VariantMap& eventData)
    {
        using namespace NETBuildAtomicProject;

        Project* project = static_cast<Project*>(eventData[P_PROJECT].GetPtr());

        if (!project)
        {
            LOGERROR("NETBuildSystem::HandleBuildAtomicProject - null project");
            return;
        }

        String platform;
        String configuration;

#ifdef ATOMIC_PLATFORM_WINDOWS
        platform = "WINDOWS";
#elif ATOMIC_PLATFORM_OSX
        platform = "MACOSX";
#else
        platform = "LINUX";
#endif

#ifdef _DEBUG
        configuration = "Debug";
#else
        configuration = "Release";
#endif

        String projectPath = project->GetProjectFilePath();

        NETBuild* build = Build(projectPath, platform, configuration);

        if (build)
        {
            build->project_ = project;
        }

        LOGINFOF("Received build for project %s", project->GetProjectFilePath().CString());
            
    }

    NETBuild* NETBuildSystem::Build(const String& solutionPath, const String& platform, const String& configuration)
    {

        FileSystem* fileSystem = GetSubsystem<FileSystem>();

        if (!fileSystem->FileExists(solutionPath))
        {
            LOGERRORF("NETBuildSystem::Build - Solution does not exist (%s)", solutionPath.CString());
            return 0;
        }

        // Get existing build
        SharedPtr<NETBuild> build(GetBuild(solutionPath, platform, configuration));

        if (build.NotNull())
            return build;

        // Create a new build
        build = new NETBuild(context_, solutionPath, platform, configuration);

        builds_.Push(build);        

        return build;
    }


}
