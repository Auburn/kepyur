# Play RX's changes to the wifi-sdr and devourer submodules (patches/README.md), applied to their working trees
# at configure time. Idempotent: a patch that is already in is left alone, so configuring again changes nothing.
# A patch that neither applies nor is already in means the submodule pin moved without the patch being
# regenerated; that stops the configure rather than building an adapter driver without the fixes.
find_package(Git REQUIRED)

# marker: a piece of text the patch adds to marker_file (a path inside dir), checked whenever git says the patch
# is already in.
function(kp_apply_patch dir patch marker_file marker)
  get_filename_component(name ${patch} NAME)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${patch})  # an edited patch re-runs this
  # git must look for a repository in ${dir} and no further up. Where ${dir} is not a work tree of its own but a
  # plain directory inside some other repository (kepyur's sources vendored into one, or unpacked from an archive
  # inside a checkout), git apply takes the patch's paths as relative to that repository's top, finds none of
  # them under ${dir}, skips them all and reports success, for the reverse check too: the patch then read as
  # already in and nothing was patched. With no repository in sight git apply is a plain patch tool.
  get_filename_component(dir ${dir} REALPATH)
  get_filename_component(above ${dir} DIRECTORY)
  set(git ${CMAKE_COMMAND} -E env GIT_CEILING_DIRECTORIES=${above} ${GIT_EXECUTABLE} -C ${dir})
  # Two configures at once (Gradle's variants, a desktop and a web build) must not both apply it: hold a lock in
  # the submodule's git dir (outside its working tree, so it shows in no status) while checking and applying.
  execute_process(COMMAND ${git} rev-parse --absolute-git-dir
                  OUTPUT_VARIABLE gitdir OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE grc ERROR_QUIET)
  if(grc EQUAL 0 AND IS_DIRECTORY "${gitdir}")
    file(LOCK "${gitdir}/kepyur-patch.lock" GUARD FUNCTION TIMEOUT 120)
  endif()
  # A .git in ${dir} that leads to no repository: a copy of a checkout made without the top .git directory (a
  # build context that leaves it out, an unpacked backup) still has the submodule's .git file, and git refuses
  # to do anything there. From the directory above, with no repository in sight from it, git apply is the same
  # plain patch tool and never looks at that file.
  set(into "")
  if(NOT grc EQUAL 0 AND EXISTS "${dir}/.git")
    get_filename_component(leaf ${dir} NAME)
    get_filename_component(above2 ${above} DIRECTORY)
    set(git ${CMAKE_COMMAND} -E env GIT_CEILING_DIRECTORIES=${above2} ${GIT_EXECUTABLE} -C ${above})
    set(into --directory=${leaf})
  endif()
  # already applied: the patch reverses cleanly
  execute_process(COMMAND ${git} apply ${into} --reverse --check ${patch}
                  RESULT_VARIABLE rev OUTPUT_QUIET ERROR_QUIET)
  if(NOT rev EQUAL 0)
    execute_process(COMMAND ${git} apply ${into} --check ${patch}
                    RESULT_VARIABLE fwd OUTPUT_QUIET ERROR_VARIABLE fwd_err)
    if(NOT fwd EQUAL 0)
      execute_process(COMMAND ${git} rev-parse --short HEAD
                      OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      message(FATAL_ERROR "patch ${name} neither applies to nor is already in ${dir} (at commit ${head}): the "
                          "submodule pin and the patch are out of step. Regenerate the patch against this commit "
                          "(patches/README.md) or check the submodule out at the commit it was made for.\n${fwd_err}")
    endif()
    execute_process(COMMAND ${git} apply ${into} ${patch} RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "patch ${name}: git apply failed in ${dir}:\n${err}")
    endif()
    message(STATUS "applied ${name} to ${dir}")
  endif()
  # Do not take git's word alone for it (see above): the patched text has to be there.
  set(text "")
  if(EXISTS ${dir}/${marker_file})
    file(READ ${dir}/${marker_file} text)
  endif()
  string(FIND "${text}" "${marker}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "patch ${name}: git reports it as applied in ${dir}, but ${marker_file} there does not "
                        "have \"${marker}\", which the patch adds. The sources would be built without the "
                        "patch; apply it by hand (patches/README.md).")
  endif()
endfunction()

# KEPYUR_ROOT: the repository root (native/CMakeLists.txt and web/CMakeLists.txt set it)
kp_apply_patch(${KEPYUR_ROOT}/third_party/wifi-sdr ${KEPYUR_ROOT}/patches/wifi-sdr-0001-playrx-stream.patch
               src/receiver.h "double centre = 0;")
kp_apply_patch(${KEPYUR_ROOT}/third_party/wifi-sdr/external/devourer
               ${KEPYUR_ROOT}/patches/devourer-0001-playrx-usb.patch
               src/UsbTransport.h "kCtrlDeadAfter")
