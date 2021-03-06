// options.cpp

// Implementation of options parsing for `slangc` command line,
// and also for API interface that takes command-line argument strings.

#include "../../slang.h"

#include "compiler.h"
#include "profile.h"

#include <assert.h>

namespace Slang {

SlangResult tryReadCommandLineArgumentRaw(DiagnosticSink* sink, char const* option, char const* const**ioCursor, char const* const*end, char const** argOut)
{
    *argOut = nullptr;
    char const* const*& cursor = *ioCursor;
    if (cursor == end)
    {
        sink->diagnose(SourceLoc(), Diagnostics::expectedArgumentForOption, option);
        return SLANG_FAIL;
    }
    else
    {
        *argOut = *cursor++;
        return SLANG_OK;
    }
}

SlangResult tryReadCommandLineArgument(DiagnosticSink* sink, char const* option, char const* const**ioCursor, char const* const*end, String& argOut)
{
    const char* arg;
    SLANG_RETURN_ON_FAIL(tryReadCommandLineArgumentRaw(sink, option, ioCursor, end, &arg));
    argOut = arg;
    return SLANG_OK;
}

struct OptionsParser
{
    SlangSession*           session = nullptr;
    SlangCompileRequest*    compileRequest = nullptr;

    Slang::CompileRequest*  requestImpl = nullptr;

    struct RawTranslationUnit
    {
        SlangSourceLanguage sourceLanguage;
        SlangProfileID      implicitProfile;
        int                 translationUnitIndex;
    };

    // Collect translation units so that we can futz with them later
    List<RawTranslationUnit> rawTranslationUnits;

    struct RawEntryPoint
    {
        String          name;
        SlangProfileID  profileID = SLANG_PROFILE_UNKNOWN;
        int             translationUnitIndex = -1;
        int             outputPathIndex = -1;
    };

    // Collect entry point names, so that we can associate them
    // with entry points later...
    List<RawEntryPoint> rawEntryPoints;

    // The number of input files that have been specified
    int inputPathCount = 0;

    // If we already have a translation unit for Slang code, then this will give its index.
    // If not, it will be `-1`.
    int slangTranslationUnit = -1;

    int translationUnitCount = 0;
    int currentTranslationUnitIndex = -1;

    SlangProfileID currentProfileID = SLANG_PROFILE_UNKNOWN;

    // How many times were `-profile` options given?
    int profileOptionCount = 0;

    SlangCompileFlags flags = 0;
    SlangTargetFlags targetFlags = 0;

    struct RawOutputPath
    {
        String              path;
        SlangCompileTarget  target;
    };

    List<RawOutputPath> rawOutputPaths;

    SlangCompileTarget chosenTarget = SLANG_TARGET_NONE;

    int addTranslationUnit(
        SlangSourceLanguage language,
        SlangProfileID      implicitProfile = SLANG_PROFILE_UNKNOWN)
    {
        auto translationUnitIndex = spAddTranslationUnit(compileRequest, language, nullptr);

        SLANG_RELEASE_ASSERT(UInt(translationUnitIndex) == rawTranslationUnits.Count());

        RawTranslationUnit rawTranslationUnit;
        rawTranslationUnit.sourceLanguage = language;
        rawTranslationUnit.implicitProfile = implicitProfile;
        rawTranslationUnit.translationUnitIndex = translationUnitIndex;

        rawTranslationUnits.Add(rawTranslationUnit);

        return translationUnitIndex;
    }

    void addInputSlangPath(
        String const& path)
    {
        // All of the input .slang files will be grouped into a single logical translation unit,
        // which we create lazily when the first .slang file is encountered.
        if( slangTranslationUnit == -1 )
        {
            translationUnitCount++;
            slangTranslationUnit = addTranslationUnit(SLANG_SOURCE_LANGUAGE_SLANG);
        }

        spAddTranslationUnitSourceFile(
            compileRequest,
            slangTranslationUnit,
            path.begin());

        // Set the translation unit to be used by subsequent entry points
        currentTranslationUnitIndex = slangTranslationUnit;
    }

    void addInputForeignShaderPath(
        String const&           path,
        SlangSourceLanguage     language,
        SlangProfileID          implicitProfile = SLANG_PROFILE_UNKNOWN)
    {
        translationUnitCount++;
        currentTranslationUnitIndex = addTranslationUnit(language, implicitProfile);

        spAddTranslationUnitSourceFile(
            compileRequest,
            currentTranslationUnitIndex,
            path.begin());
    }

    static Profile::RawVal findGlslProfileFromPath(const String& path)
    {
        struct Entry
        {
            const char* ext;
            Profile::RawVal profileId;
        };

        static const Entry entries[] = 
        {
            { ".frag", Profile::GLSL_Fragment },
            { ".geom", Profile::GLSL_Geometry },
            { ".tesc", Profile::GLSL_TessControl },
            { ".tese", Profile::GLSL_TessEval },
            { ".comp", Profile::GLSL_Compute } 
        };

        for (int i = 0; i < SLANG_COUNT_OF(entries); ++i)
        {
            const Entry& entry = entries[i];
            if (path.EndsWith(entry.ext))
            {
                return entry.profileId;
            }
        }
        return Profile::Unknown;
    }

    static SlangSourceLanguage findSourceLanguageFromPath(const String& path, SlangProfileID* profileOut)
    {
        *profileOut = SLANG_PROFILE_UNKNOWN;

        if (path.EndsWith(".hlsl") ||
            path.EndsWith(".fx"))
        {
            return SLANG_SOURCE_LANGUAGE_HLSL;
        }
        if (path.EndsWith(".glsl"))
        {
            return SLANG_SOURCE_LANGUAGE_GLSL;
        }

        Profile::RawVal profile = findGlslProfileFromPath(path);
        if (profile != Profile::Unknown)
        {
            *profileOut = SlangProfileID(profile);
            return SLANG_SOURCE_LANGUAGE_GLSL;
        }
        return SLANG_SOURCE_LANGUAGE_UNKNOWN;
    }

    SlangResult addInputPath(
        char const*  inPath)
    {
        inputPathCount++;

        // look at the extension on the file name to determine
        // how we should handle it.
        String path = String(inPath);

        if( path.EndsWith(".slang") )
        {
            // Plain old slang code
            addInputSlangPath(path);
            return SLANG_OK;
        }
        
        SlangProfileID profileID;
        SlangSourceLanguage sourceLanguage = findSourceLanguageFromPath(path, &profileID);
        
        if (sourceLanguage == SLANG_SOURCE_LANGUAGE_UNKNOWN)
        {
            requestImpl->mSink.diagnose(SourceLoc(), Diagnostics::cannotDeduceSourceLanguage, inPath);
            return SLANG_FAIL;
        }

        addInputForeignShaderPath(path, sourceLanguage, profileID);

        return SLANG_OK;
    }

    void addOutputPath(
        String const&       path,
        SlangCompileTarget  target)
    {
        RawOutputPath rawOutputPath;
        rawOutputPath.path = path;
        rawOutputPath.target = target;

        rawOutputPaths.Add(rawOutputPath);
    }

    void addOutputPath(char const* inPath)
    {
        String path = String(inPath);

        if (!inPath) {}
#define CASE(EXT, TARGET)   \
        else if(path.EndsWith(EXT)) do { addOutputPath(path, SLANG_##TARGET); } while(0)

        CASE(".hlsl", HLSL);
        CASE(".fx",   HLSL);

        CASE(".dxbc", DXBC);
        CASE(".dxbc.asm", DXBC_ASM);

        CASE(".glsl", GLSL);
        CASE(".vert", GLSL);
        CASE(".frag", GLSL);
        CASE(".geom", GLSL);
        CASE(".tesc", GLSL);
        CASE(".tese", GLSL);
        CASE(".comp", GLSL);

        CASE(".spv",        SPIRV);
        CASE(".spv.asm",    SPIRV_ASM);

#undef CASE

        else if (path.EndsWith(".slang-module"))
        {
            spSetOutputContainerFormat(compileRequest, SLANG_CONTAINER_FORMAT_SLANG_MODULE);
            requestImpl->containerOutputPath = path;
        }
        else
        {
            // Allow an unknown-format `-o`, assuming we get a target format
            // from another argument.
            addOutputPath(path, SLANG_TARGET_UNKNOWN);
        }
    }

    SlangResult parse(
        int             argc,
        char const* const*  argv)
    {
        // Copy some state out of the current request, in case we've been called
        // after some other initialization has been performed.
        flags = requestImpl->compileFlags;

        DiagnosticSink* sink = &requestImpl->mSink;

        SlangMatrixLayoutMode defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_MODE_UNKNOWN;

        char const* const* argCursor = &argv[0];
        char const* const* argEnd = &argv[argc];
        while (argCursor != argEnd)
        {
            char const* arg = *argCursor++;
            if (arg[0] == '-')
            {
                String argStr = String(arg);

                // The argument looks like an option, so try to parse it.
//                if (argStr == "-outdir")
//                    outputDir = tryReadCommandLineArgument(arg, &argCursor, argEnd);
//                if (argStr == "-out")
//                    options.outputName = tryReadCommandLineArgument(arg, &argCursor, argEnd);
//                else if (argStr == "-symbo")
//                    options.SymbolToCompile = tryReadCommandLineArgument(arg, &argCursor, argEnd);
                //else
                if(argStr == "-no-mangle" )
                {
                    flags |= SLANG_COMPILE_FLAG_NO_MANGLING;
                }
                else if (argStr == "-no-codegen")
                {
                    flags |= SLANG_COMPILE_FLAG_NO_CODEGEN;
                }
                else if(argStr == "-dump-ir" )
                {
                    requestImpl->shouldDumpIR = true;
                }
                else if(argStr == "-validate-ir" )
                {
                    requestImpl->shouldValidateIR = true;
                }
                else if(argStr == "-skip-codegen" )
                {
                    requestImpl->shouldSkipCodegen = true;
                }
                else if(argStr == "-parameter-blocks-use-register-spaces" )
                {
                    targetFlags |= SLANG_TARGET_FLAG_PARAMETER_BLOCKS_USE_REGISTER_SPACES;
                }
                else if (argStr == "-backend" || argStr == "-target")
                {
                    String name;
                    SLANG_RETURN_ON_FAIL(tryReadCommandLineArgument(sink, arg, &argCursor, argEnd, name));

                    SlangCompileTarget target = SLANG_TARGET_UNKNOWN;

                    if (name == "glsl")
                    {
                        target = SLANG_GLSL;
                    }
                    else if (name == "glsl_vk")
                    {
                        target = SLANG_GLSL_VULKAN;
                    }
//                    else if (name == "glsl_vk_onedesc")
//                    {
//                        options.Target = CodeGenTarget::GLSL_Vulkan_OneDesc;
//                    }
                    else if (name == "hlsl")
                    {
                        target = SLANG_HLSL;
                    }
                    else if (name == "spriv")
                    {
                        target = SLANG_SPIRV;
                    }
                    else if (name == "dxbc")
                    {
                        target = SLANG_DXBC;
                    }
                    else if (name == "dxbc-assembly")
                    {
                        target = SLANG_DXBC_ASM;
                    }
                #define CASE(NAME, TARGET)  \
                    else if(name == #NAME) do { target = SLANG_##TARGET; } while(0)

                    CASE(spirv, SPIRV);
                    CASE(spirv-assembly, SPIRV_ASM);
                    CASE(dxil, DXIL);
                    CASE(dxil-assembly, DXIL_ASM);
                    CASE(none, TARGET_NONE);

                #undef CASE

                    else
                    {
                        sink->diagnose(SourceLoc(), Diagnostics::unknownCodeGenerationTarget, name);
                        return SLANG_FAIL;
                    }

                    this->chosenTarget = target;
                    spSetCodeGenTarget(compileRequest, target);
                }
                // A "profile" specifies both a specific target stage and a general level
                // of capability required by the program.
                else if (argStr == "-profile")
                {
                    String name;
                    SLANG_RETURN_ON_FAIL(tryReadCommandLineArgument(sink, arg, &argCursor, argEnd, name));

                    SlangProfileID profileID = spFindProfile(session, name.begin());
                    if( profileID == SLANG_PROFILE_UNKNOWN )
                    {
                        sink->diagnose(SourceLoc(), Diagnostics::unknownProfile, name);
                        return SLANG_FAIL;
                    }
                    else
                    {
                        currentProfileID = profileID;
                        profileOptionCount++;
                    }
                }
                else if (argStr == "-entry")
                {
                    String name;
                    SLANG_RETURN_ON_FAIL(tryReadCommandLineArgument(sink, arg, &argCursor, argEnd, name));

                    RawEntryPoint entry;
                    entry.name = name;
                    entry.translationUnitIndex = currentTranslationUnitIndex;

                    int outputPathCount = (int) rawOutputPaths.Count();
                    int currentOutputPathIndex = outputPathCount - 1;
                    entry.outputPathIndex = currentOutputPathIndex;

                    // TODO(tfoley): Allow user to fold a specification of a profile into the entry-point name,
                    // for the case where they might be compiling multiple entry points in one invocation...
                    //
                    // For now, just use the last profile set on the command-line to specify this

                    entry.profileID = currentProfileID;

                    rawEntryPoints.Add(entry);
                }
#if 0
                else if (argStr == "-stage")
                {
                    String name;
                    SLANG_RETURN_ON_FAIL(tryReadCommandLineArgument(sink, arg, &argCursor, argEnd, name));

                    StageTarget stage = StageTarget::Unknown;
                    if (name == "vertex") { stage = StageTarget::VertexShader; }
                    else if (name == "fragment") { stage = StageTarget::FragmentShader; }
                    else if (name == "hull") { stage = StageTarget::HullShader; }
                    else if (name == "domain") { stage = StageTarget::DomainShader; }
                    else if (name == "compute") { stage = StageTarget::ComputeShader; }
                    else
                    {
                        sink->diagnose(SourceLoc(), Diagnostics::unknownStage, name);
                        return SLANG_FAIL;
                    }
                    options.stage = stage;
                }
#endif
                else if (argStr == "-pass-through")
                {
                    String name;
                    SLANG_RETURN_ON_FAIL(tryReadCommandLineArgument(sink, arg, &argCursor, argEnd, name));

                    SlangPassThrough passThrough = SLANG_PASS_THROUGH_NONE;
                    if (name == "fxc") { passThrough = SLANG_PASS_THROUGH_FXC; }
                    else if (name == "dxc") { passThrough = SLANG_PASS_THROUGH_DXC; }
                    else if (name == "glslang") { passThrough = SLANG_PASS_THROUGH_GLSLANG; }
                    else
                    {
                        sink->diagnose(SourceLoc(), Diagnostics::unknownPassThroughTarget, name);
                        return SLANG_FAIL;
                    }

                    spSetPassThrough(
                        compileRequest,
                        passThrough);
                }
//                else if (argStr == "-genchoice")
//                    options.Mode = CompilerMode::GenerateChoice;
                else if (argStr[1] == 'D')
                {
                    // The value to be defined might be part of the same option, as in:
                    //     -DFOO
                    // or it might come separately, as in:
                    //     -D FOO
                    char const* defineStr = arg + 2;
                    if (defineStr[0] == 0)
                    {
                        // Need to read another argument from the command line
                        SLANG_RETURN_ON_FAIL(tryReadCommandLineArgumentRaw(sink, arg, &argCursor, argEnd, &defineStr));
                    }
                    // The string that sets up the define can have an `=` between
                    // the name to be defined and its value, so we search for one.
                    char const* eqPos = nullptr;
                    for(char const* dd = defineStr; *dd; ++dd)
                    {
                        if (*dd == '=')
                        {
                            eqPos = dd;
                            break;
                        }
                    }

                    // Now set the preprocessor define
                    //
                    if (eqPos)
                    {
                        // If we found an `=`, we split the string...

                        spAddPreprocessorDefine(
                            compileRequest,
                            String(defineStr, eqPos).begin(),
                            String(eqPos+1).begin());
                    }
                    else
                    {
                        // If there was no `=`, then just #define it to an empty string

                        spAddPreprocessorDefine(
                            compileRequest,
                            String(defineStr).begin(),
                            "");
                    }
                }
                else if (argStr[1] == 'I')
                {
                    // The value to be defined might be part of the same option, as in:
                    //     -IFOO
                    // or it might come separately, as in:
                    //     -I FOO
                    // (see handling of `-D` above)
                    char const* includeDirStr = arg + 2;
                    if (includeDirStr[0] == 0)
                    {
                        // Need to read another argument from the command line
                        SLANG_RETURN_ON_FAIL(tryReadCommandLineArgumentRaw(sink, arg, &argCursor, argEnd, &includeDirStr));
                    }

                    spAddSearchPath(
                        compileRequest,
                        String(includeDirStr).begin());
                }
                //
                // A `-o` option is used to specify a desired output file.
                else if (argStr == "-o")
                {
                    char const* outputPath = nullptr;
                    SLANG_RETURN_ON_FAIL(tryReadCommandLineArgumentRaw(sink, arg, &argCursor, argEnd, &outputPath));
                    if (!outputPath) continue;

                    addOutputPath(outputPath);
                }
                else if(argStr == "-matrix-layout-row-major")
                {
                    defaultMatrixLayoutMode = kMatrixLayoutMode_RowMajor;
                }
                else if(argStr == "-matrix-layout-column-major")
                {
                    defaultMatrixLayoutMode = kMatrixLayoutMode_ColumnMajor;
                }
                else if (argStr == "--")
                {
                    // The `--` option causes us to stop trying to parse options,
                    // and treat the rest of the command line as input file names:
                    while (argCursor != argEnd)
                    {
                        SLANG_RETURN_ON_FAIL(addInputPath(*argCursor++));
                    }
                    break;
                }
                else
                {
                    sink->diagnose(SourceLoc(), Diagnostics::unknownCommandLineOption, argStr);
                    // TODO: print a usage message
                    return SLANG_FAIL;
                }
            }
            else
            {
                SLANG_RETURN_ON_FAIL(addInputPath(arg));
            }
        }

        spSetCompileFlags(compileRequest, flags);

        // TODO(tfoley): This kind of validation needs to wait until
        // after all options have been specified for API usage
#if 0
        if (inputPathCount == 0)
        {
            fprintf(stderr, "error: no input file specified\n");
            return SLANG_E_INVALID_ARG; 
        }

        // No point in moving forward if there is nothing to compile
        if( translationUnitCount == 0 )
        {
            fprintf(stderr, "error: no compilation requested\n");
            return SLANG_FAIL; 
        }
#endif

        // If the user didn't list any explicit entry points, then we can
        // try to infer one from the type of input file
        if(rawEntryPoints.Count() == 0)
        {
            for(auto rawTranslationUnit : rawTranslationUnits)
            {
                // Dont' add implicit entry points when compiling from Slang files,
                // since Slang doesn't require entry points to be named on the
                // command line.
                if(rawTranslationUnit.sourceLanguage == SLANG_SOURCE_LANGUAGE_SLANG )
                    continue;

                // Use a default entry point name
                char const* entryPointName = "main";

                // Try to determine a profile
                SlangProfileID entryPointProfile = SLANG_PROFILE_UNKNOWN;

                // If a profile was specified on the command line, then we use it
                if(currentProfileID != SLANG_PROFILE_UNKNOWN)
                {
                    entryPointProfile = currentProfileID;
                }
                // Otherwise, check if the translation unit implied a profile
                // (e.g., a `*.vert` file implies the `GLSL_Vertex` profile)
                else if(rawTranslationUnit.implicitProfile != SLANG_PROFILE_UNKNOWN)
                {
                    entryPointProfile = rawTranslationUnit.implicitProfile;
                }

                RawEntryPoint entry;
                entry.name = entryPointName;
                entry.translationUnitIndex = rawTranslationUnit.translationUnitIndex;
                entry.profileID = entryPointProfile;
                rawEntryPoints.Add(entry);
            }
        }

        // For any entry points that were given without an explicit profile, we can now apply
        // the profile that was given to them.
        if( rawEntryPoints.Count() != 0 )
        {
            bool anyEntryPointWithoutProfile = false;
            for( auto& entryPoint : rawEntryPoints )
            {
                // Skip entry points that are already associated with a translation unit...
                if( entryPoint.profileID != SLANG_PROFILE_UNKNOWN )
                    continue;

                anyEntryPointWithoutProfile = true;
                break;
            }

            // Issue an error if there are entry points without a profile,
            // and no profile was specified.
            if( anyEntryPointWithoutProfile
                && currentProfileID == SLANG_PROFILE_UNKNOWN)
            {
                sink->diagnose(SourceLoc(), Diagnostics::noProfileSpecified);
                return SLANG_E_INVALID_ARG;
            }
            // Issue an error if we have mulitple `-profile` options *and*
            // there were entry points that didn't get a profile, *and*
            // there we m
            if (anyEntryPointWithoutProfile
                && profileOptionCount > 1)
            {
                if (rawEntryPoints.Count() > 1)
                {
                    sink->diagnose(SourceLoc(), Diagnostics::multipleEntryPointsNeedMulitpleProfiles);
                    return SLANG_E_INVALID_ARG;
                }
            }
            // TODO: need to issue an error on a `-profile` option that doesn't actually
            // affect any entry point...

            // Take the profile that was specified on the command line, and
            // apply it to any entry points that don't already have a profile.
            for( auto& e : rawEntryPoints )
            {
                if( e.profileID == SLANG_PROFILE_UNKNOWN )
                {
                    e.profileID = currentProfileID;
                }
            }
        }

        // If the user is requesting multiple targets, *and* is asking
        // for direct output files for entry points, that is an error.
        if (rawOutputPaths.Count() != 0 && requestImpl->targets.Count() > 1)
        {
            sink->diagnose(SourceLoc(), Diagnostics::explicitOutputPathsAndMultipleTargets);
        }

        // Did the user try to specify output path(s)?
        if (rawOutputPaths.Count() != 0)
        {
            if (rawEntryPoints.Count() == 1 && rawOutputPaths.Count() == 1)
            {
                // There was exactly one entry point, and exactly one output path,
                // so we can directly use that path for the entry point.
                rawEntryPoints[0].outputPathIndex = 0;
            }
            else if (rawOutputPaths.Count() > rawEntryPoints.Count())
            {
                sink->diagnose(SourceLoc(), Diagnostics::tooManyOutputPathsSpecified,
                    rawOutputPaths.Count(), rawEntryPoints.Count());
            }
            else
            {
                // If the user tried to apply explicit output paths, but there
                // were any entry points that didn't pick up a path, that is
                // an error:
                for( auto& entryPoint : rawEntryPoints )
                {
                    if (entryPoint.outputPathIndex < 0)
                    {
                        sink->diagnose(SourceLoc(), Diagnostics::noOutputPathSpecifiedForEntryPoint, entryPoint.name);

                        // Don't emit this same error for other entry
                        // points, even if we have more
                        break;
                    }
                }
            }

            // All of the output paths had better agree on the format
            // they should provide.
            switch (chosenTarget)
            {
            case SLANG_TARGET_NONE:
            case SLANG_TARGET_UNKNOWN:
                // No direct `-target` argument given, so try to infer
                // a target from the entry points:
                {
                    bool anyUnknownTargets = false;
                    for (auto rawOutputPath : rawOutputPaths)
                    {
                        if (rawOutputPath.target == SLANG_TARGET_UNKNOWN)
                        {
                            // This file didn't imply a target, and that
                            // needs to be an error:
                            sink->diagnose(SourceLoc(), Diagnostics::cannotDeduceOutputFormatFromPath, rawOutputPath.path);

                            // Don't keep looking for errors
                            anyUnknownTargets = true;
                            break;
                        }
                    }

                    if (!anyUnknownTargets)
                    {
                        // Okay, all the files have explicit targets,
                        // so we will set the code generation target
                        // accordingly, and then ensure that all
                        // the other output paths specified (if any)
                        // are consistent with the chosen target.
                        //
                        auto target = rawOutputPaths[0].target;
                        spSetCodeGenTarget(
                            compileRequest,
                            target);

                        for (auto rawOutputPath : rawOutputPaths)
                        {
                            if (rawOutputPath.target != target)
                            {
                                // This file didn't imply a target, and that
                                // needs to be an error:
                                sink->diagnose(
                                    SourceLoc(),
                                    Diagnostics::outputPathsImplyDifferentFormats,
                                    rawOutputPaths[0].path,
                                    rawOutputPath.path);

                                // Don't keep looking for errors
                                break;
                            }
                        }
                    }

                }
                break;

            default:
                {
                    // An explicit target was given on the command-line.
                    // We will trust that the user knows what they are
                    // doing, even if one of the output files implies
                    // a different format.
                }
                break;

            }
        }

        // If the user specified and per-compilation-target flags, make sure
        // to apply them here.
        if(targetFlags)
        {
            spSetTargetFlags(compileRequest, 0, targetFlags);
        }

        if(defaultMatrixLayoutMode != SLANG_MATRIX_LAYOUT_MODE_UNKNOWN)
        {
            UInt targetCount = requestImpl->targets.Count();
            for(UInt tt = 0; tt < targetCount; ++tt)
            {
                spSetTargetMatrixLayoutMode(compileRequest, int(tt), defaultMatrixLayoutMode);
            }
        }

        // Next, we want to make sure that entry points get attached to the appropriate translation
        // unit that will provide them.
        {
            bool anyEntryPointWithoutTranslationUnit = false;
            for( auto& entryPoint : rawEntryPoints )
            {
                // Skip entry points that are already associated with a translation unit...
                if( entryPoint.translationUnitIndex != -1 )
                    continue;

                anyEntryPointWithoutTranslationUnit = true;
                entryPoint.translationUnitIndex = 0;
            }

            if( anyEntryPointWithoutTranslationUnit && translationUnitCount != 1 )
            {
                sink->diagnose(SourceLoc(), Diagnostics::multipleTranslationUnitsNeedEntryPoints);
                return SLANG_FAIL;
            }

            // Now place all those entry points where they belong
            for( auto& entryPoint : rawEntryPoints )
            {
                int entryPointIndex = spAddEntryPoint(
                    compileRequest,
                    entryPoint.translationUnitIndex,
                    entryPoint.name.begin(),
                    entryPoint.profileID);

                // If an output path was specified for the entry point,
                // when we need to provide it here.
                if (entryPoint.outputPathIndex >= 0)
                {
                    auto rawOutputPath = rawOutputPaths[entryPoint.outputPathIndex];

                    requestImpl->entryPoints[entryPointIndex]->outputPath = rawOutputPath.path;
                }
            }
        }

#if 0
        // Automatically derive an output directory based on the first file specified.
        //
        // TODO: require manual specification if there are multiple input files, in different directories
        String fileName = options.translationUnits[0].sourceFilePaths[0];
        if (outputDir.Length() == 0)
        {
            outputDir = Path::GetDirectoryName(fileName);
        }
#endif

        return (sink->GetErrorCount() == 0) ? SLANG_OK : SLANG_FAIL;
    }
};


SlangResult parseOptions(
    SlangCompileRequest*    compileRequestIn,
    int                     argc,
    char const* const*      argv)
{
    Slang::CompileRequest* compileRequest = (Slang::CompileRequest*) compileRequestIn;

    OptionsParser parser;
    parser.compileRequest = compileRequestIn;
    parser.requestImpl = compileRequest;

    Result res = parser.parse(argc, argv);

    DiagnosticSink* sink = &compileRequest->mSink;
    if (sink->GetErrorCount() > 0)
    {
        // Put the errors in the diagnostic 
        compileRequest->mDiagnosticOutput = sink->outputBuffer.ProduceString();
    }

    return res;
}


} // namespace Slang

SLANG_API SlangResult spProcessCommandLineArguments(
    SlangCompileRequest*    request,
    char const* const*      args,
    int                     argCount)
{
    return Slang::parseOptions(request, argCount, args);
}
