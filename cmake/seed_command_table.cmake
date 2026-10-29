# 种子复制：仅当 exe 旁不存在 debug_commands.json 时才复制一次。
# 用户通过「编辑指令」保存的定制指令表不再被后续构建覆盖（真机教训：
# 构建覆盖曾把用户的全部自定义指令冲掉）。
if(NOT EXISTS "${dst}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${src}" "${dst}")
endif()
