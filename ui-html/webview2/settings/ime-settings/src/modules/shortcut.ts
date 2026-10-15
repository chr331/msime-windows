import { updateConfig } from './config-sync';

const DEFAULT_PUNCTUATION_HOTKEY = 'Ctrl+Period';
const PUNCTUATION_KEYS: Record<string, string> = {
  Semicolon: ';', Equal: '=', Comma: ',', Minus: '-', Period: '.', Slash: '/',
  Backquote: '`', BracketLeft: '[', Backslash: '\\', BracketRight: ']', Quote: "'",
};
let punctuationHotkey = DEFAULT_PUNCTUATION_HOTKEY;

function formatPunctuationHotkey(binding: string): string {
  return binding ? binding.split('+').map(key => PUNCTUATION_KEYS[key] ??
    (key === 'Alt' ? '左 Alt' : key.replace(/^(Key|Digit)/, ''))).join(' + ') : '未绑定';
}

function displayPunctuationHotkey(): void {
  const button = document.getElementById('punctuationHotkeyButton');
  if (button && button.getAttribute('aria-pressed') !== 'true') {
    button.textContent = formatPunctuationHotkey(punctuationHotkey);
  }
}

function punctuationEventCode(event: KeyboardEvent): string {
  // code identifies the physical key; WebView2 keyCode carries the Windows VK
  // used by TSF, including when a keyboard layout remaps letters or OEM keys.
  if (/^(Control|Shift|Alt|Meta)(Left|Right)$/.test(event.code)) return event.code;
  if (event.code.startsWith('Numpad') || event.location === 3) return '';
  const vk = event.keyCode;
  if (vk >= 65 && vk <= 90) return `Key${String.fromCharCode(vk)}`;
  if (vk >= 48 && vk <= 57) return `Digit${vk - 48}`;
  if (vk >= 112 && vk <= 123) return `F${vk - 111}`;
  return ({
    9: 'Tab', 16: 'ShiftLeft', 17: 'ControlLeft', 18: 'AltLeft', 27: 'Escape',
    91: 'MetaLeft', 92: 'MetaRight', 186: 'Semicolon', 187: 'Equal', 188: 'Comma',
    189: 'Minus', 190: 'Period', 191: 'Slash', 192: 'Backquote', 219: 'BracketLeft',
    220: 'Backslash', 221: 'BracketRight', 222: 'Quote',
  } as Record<number, string>)[vk] ?? '';
}

function recordedPunctuationHotkey(event: KeyboardEvent, code: string): string | null {
  if (event.metaKey || event.ctrlKey === event.altKey || event.getModifierState('AltGraph')) return null;
  if (!/^(Key[A-Z]|Digit[0-9]|F([1-9]|1[0-2]))$/.test(code) && !(code in PUNCTUATION_KEYS)) return null;
  const binding = `${event.ctrlKey ? 'Ctrl' : 'Alt'}+${event.shiftKey ? 'Shift+' : ''}${code}`;
  return binding;
}

const PUNCTUATION_HOTKEY_CONFLICTS: Record<string, string> = {
  'Ctrl+Shift+KeyE': 'Ctrl + Shift + E 用于切换英文候选模式。',
  'Ctrl+Shift+KeyF': 'Ctrl + Shift + F 用于简繁切换。',
  'Ctrl+F9': 'Ctrl + F9 用于语音输入。',
  'Ctrl+Shift+F9': 'Ctrl + Shift + F9 用于语音输入。',
  'Alt+F4': '左 Alt + F4 用于关闭窗口。',
  'Alt+Shift+F4': '左 Alt + Shift + F4 用于关闭窗口。',
};

const COMMON_APPLICATION_SHORTCUTS: Record<string, string> = {
  'Ctrl+KeyA': '全选', 'Ctrl+KeyC': '复制', 'Ctrl+KeyV': '粘贴', 'Ctrl+KeyX': '剪切',
  'Ctrl+KeyZ': '撤销', 'Ctrl+KeyY': '重做', 'Ctrl+KeyS': '保存', 'Ctrl+KeyF': '查找',
  'Ctrl+KeyH': '替换', 'Ctrl+KeyP': '打印', 'Ctrl+KeyO': '打开', 'Ctrl+KeyN': '新建',
  'Ctrl+KeyW': '关闭标签页或文档', 'Ctrl+F4': '关闭文档', 'Ctrl+KeyT': '新建标签页',
  'Ctrl+KeyL': '定位地址栏', 'Ctrl+KeyR': '刷新', 'Ctrl+KeyB': '加粗',
  'Ctrl+KeyI': '斜体', 'Ctrl+KeyU': '下划线',
  'Ctrl+Shift+KeyV': '粘贴为纯文本', 'Ctrl+Shift+KeyZ': '重做',
  'Ctrl+Shift+KeyS': '另存为', 'Ctrl+Shift+KeyT': '重新打开关闭的标签页',
};

function setupPunctuationHotkey(): void {
  const button = document.getElementById('punctuationHotkeyButton') as HTMLButtonElement | null;
  const status = document.getElementById('punctuationHotkeyStatus');
  const confirmation = document.getElementById('punctuationHotkeyConfirmation');
  if (!button) return;
  let leftAlt = false;
  let rightAlt = false;
  let pendingBinding: string | null = null;
  const finish = (message = '') => {
    leftAlt = rightAlt = false;
    pendingBinding = null;
    if (confirmation) confirmation.hidden = true;
    button.setAttribute('aria-pressed', 'false');
    displayPunctuationHotkey();
    if (status) status.textContent = message;
  };
  const save = (binding: string) => {
    finish('正在保存…');
    updateConfig('keybindings.toggle_punctuation', binding);
    // The host returns the persisted snapshot on success AND failure.
    // Keep showing the previous value until that snapshot confirms the write.
  };
  button.addEventListener('click', () => {
    if (button.getAttribute('aria-pressed') === 'true') return finish();
    finish();
    button.setAttribute('aria-pressed', 'true');
    button.textContent = '请按下快捷键';
    if (status) status.textContent = 'Ctrl 或左 Alt + 字母、数字、F1–F12、标点，可加 Shift；Esc 取消，Tab 离开。';
  });
  button.addEventListener('blur', () => {
    if (button.getAttribute('aria-pressed') === 'true') finish();
  });
  button.addEventListener('keydown', event => {
    const code = punctuationEventCode(event);
    if (pendingBinding !== null && code === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      return finish();
    }
    if (button.getAttribute('aria-pressed') !== 'true' || code === 'Tab') return;
    event.preventDefault();
    event.stopPropagation();
    if (code === 'Escape') return finish();
    if (!event.altKey) leftAlt = rightAlt = false;
    if (code === 'AltLeft' && (event.code === 'AltLeft' || event.location === 1)) leftAlt = true;
    if (code === 'AltRight' || (event.keyCode === 18 && event.location === 2)) rightAlt = true;
    if (event.repeat || /^(Control|Shift|Alt|Meta)(Left|Right)$/.test(code)) return;
    if (event.altKey && (!leftAlt || rightAlt)) {
      if (status) status.textContent = '请松开 Alt，再使用左 Alt 录制；右 Alt 用于语音输入和 AltGr。';
      return;
    }
    const binding = recordedPunctuationHotkey(event, code);
    if (binding === null) {
      if (status) status.textContent = '请选择 Ctrl 或左 Alt + 字母、数字、F1–F12、标点，可加 Shift。';
      return;
    }
    if (PUNCTUATION_HOTKEY_CONFLICTS[binding]) {
      if (status) status.textContent = PUNCTUATION_HOTKEY_CONFLICTS[binding];
      return;
    }
    const action = COMMON_APPLICATION_SHORTCUTS[binding];
    if (!action) return save(binding);
    finish(`${formatPunctuationHotkey(binding)} 与常用的“${action}”快捷键冲突。仍要绑定吗？`);
    pendingBinding = binding;
    if (confirmation) confirmation.hidden = false;
  });
  document.getElementById('punctuationHotkeyConfirm')?.addEventListener('click', () => {
    if (pendingBinding !== null) save(pendingBinding);
  });
  document.getElementById('punctuationHotkeyCancel')?.addEventListener('click', () => finish());
  confirmation?.addEventListener('keydown', event => {
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      finish();
      button.focus();
    }
  });
  button.addEventListener('keyup', event => {
    if (!event.altKey) leftAlt = rightAlt = false;
    if (event.code === 'AltLeft' || (event.keyCode === 18 && event.location === 1)) leftAlt = false;
    if (event.code === 'AltRight' || (event.keyCode === 18 && event.location === 2)) rightAlt = false;
  });
  document.getElementById('punctuationHotkeyClear')?.addEventListener('click', () => save(''));
  document.getElementById('punctuationHotkeyReset')?.addEventListener('click', () => save(DEFAULT_PUNCTUATION_HOTKEY));
}

export function applyShortcutConfig(config: {
  switch_language_shift?: boolean;
  switch_language_ctrl?: boolean;
  switch_language_ctrl_alt_space?: boolean;
  toggle_character_set_ctrl_shift_f?: boolean;
  trilingual_cycle?: boolean;
  toggle_punctuation?: string;
} | undefined): void {
  if (!config) {
    return;
  }
  const shift = document.getElementById('switchLanguageShiftCheckbox') as HTMLInputElement | null;
  const ctrl = document.getElementById('switchLanguageCtrlCheckbox') as HTMLInputElement | null;
  const ctrlAltSpace = document.getElementById('switchLanguageCtrlAltSpaceCheckbox') as HTMLInputElement | null;
  const characterSet = document.getElementById('characterSetShortcutCheckbox') as HTMLInputElement | null;
  const trilingualCycle = document.getElementById('trilingualCycleCheckbox') as HTMLInputElement | null;
  if (characterSet && typeof config.toggle_character_set_ctrl_shift_f === 'boolean') {
    characterSet.checked = config.toggle_character_set_ctrl_shift_f;
  }
  if (shift && typeof config.switch_language_shift === 'boolean') {
    shift.checked = config.switch_language_shift;
  }
  if (ctrl && typeof config.switch_language_ctrl === 'boolean') {
    ctrl.checked = config.switch_language_ctrl;
  }
  if (ctrlAltSpace && typeof config.switch_language_ctrl_alt_space === 'boolean') {
    ctrlAltSpace.checked = config.switch_language_ctrl_alt_space;
  }
  if (trilingualCycle && typeof config.trilingual_cycle === 'boolean') {
    trilingualCycle.checked = config.trilingual_cycle;
  }
  punctuationHotkey = typeof config.toggle_punctuation === 'string' ? config.toggle_punctuation : DEFAULT_PUNCTUATION_HOTKEY;
  displayPunctuationHotkey();
  const status = document.getElementById('punctuationHotkeyStatus');
  if (status?.textContent === '正在保存…') status.textContent = '';
}

export function setupShortcut(): void {
  setupPunctuationHotkey();
  const characterSet = document.getElementById('characterSetShortcutCheckbox') as HTMLInputElement | null;
  characterSet?.addEventListener('change', () => {
    updateConfig('keybindings.toggle_character_set_ctrl_shift_f', characterSet.checked);
  });
  const trilingualCycle = document.getElementById('trilingualCycleCheckbox') as HTMLInputElement | null;
  trilingualCycle?.addEventListener('change', () => {
    updateConfig('keybindings.trilingual_cycle', trilingualCycle.checked);
  });
  const mapping: Record<string, string> = {
    shift: 'keybindings.switch_language_shift',
    ctrl: 'keybindings.switch_language_ctrl',
    'ctrl-alt-space': 'keybindings.switch_language_ctrl_alt_space'
  };
  document.querySelectorAll<HTMLInputElement>('input[name="switch-language"]').forEach((checkbox) => {
    checkbox.addEventListener('change', () => {
      const path = mapping[checkbox.value];
      if (path) {
        updateConfig(path, checkbox.checked);
      }
    });
  });
}
