vim.keymap.set("n", "<leader>m", function()
  vim.cmd("rightbelow vsplit")
  if vim.fn.has("win32") == 1 then
    vim.cmd.term("powershell.exe -command ./build.ps1")
  else
    vim.cmd.term("sh ./build.sh")
  end
  vim.cmd.stopinsert()
end, { desc = "Build winspace" })
