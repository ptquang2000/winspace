local build_buf = nil
local build_win = nil

vim.keymap.set("n", "<leader>m", function()
  local cmd = vim.fn.has("win32") == 1
    and "powershell.exe -command ./build.ps1"
    or "sh ./build.sh"

  local cur_win = vim.api.nvim_get_current_win()

  -- reuse the split if it is still on screen, otherwise open one
  if build_win and vim.api.nvim_win_is_valid(build_win) then
    vim.api.nvim_set_current_win(build_win)
  else
    vim.cmd("rightbelow vsplit")
    build_win = vim.api.nvim_get_current_win()
  end

  -- a terminal job cannot be restarted in place, so drop the old buffer
  if build_buf and vim.api.nvim_buf_is_valid(build_buf) then
    vim.api.nvim_buf_delete(build_buf, { force = true })
  end

  vim.cmd.term(cmd)
  build_buf = vim.api.nvim_get_current_buf()
  vim.api.nvim_buf_set_name(build_buf, "winspace://build")
  vim.cmd.stopinsert()

  -- hand focus back to wherever you were
  if vim.api.nvim_win_is_valid(cur_win) then
    vim.api.nvim_set_current_win(cur_win)
  end
end, { desc = "Build winspace" })
