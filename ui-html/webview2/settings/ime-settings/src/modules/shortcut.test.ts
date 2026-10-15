import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyShortcutConfig, setupShortcut } from './shortcut';
import { updateConfig } from './config-sync';

vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));

class Checkbox extends EventTarget {
  checked = true;
  textContent = '';
  hidden = true;
  focus = vi.fn();
  private attributes = new Map<string, string>();
  getAttribute(name: string): string | null { return this.attributes.get(name) ?? null; }
  setAttribute(name: string, value: string): void { this.attributes.set(name, value); }
}

let characterSet: Checkbox;
let trilingualCycle: Checkbox;
let editor: Record<string, Checkbox>;
beforeEach(() => {
  vi.clearAllMocks();
  characterSet = new Checkbox();
  trilingualCycle = new Checkbox();
  trilingualCycle.checked = false;
  editor = Object.fromEntries(['punctuationHotkeyButton', 'punctuationHotkeyClear', 'punctuationHotkeyReset',
    'punctuationHotkeyStatus', 'punctuationHotkeyConfirmation', 'punctuationHotkeyConfirm',
    'punctuationHotkeyCancel'].map(id => [id, new Checkbox()]));
  vi.stubGlobal('document', {
    getElementById: (id: string) => {
      if (id === 'characterSetShortcutCheckbox') return characterSet;
      if (id === 'trilingualCycleCheckbox') return trilingualCycle;
      return editor[id] ?? null;
    },
    querySelectorAll: () => [],
  });
  applyShortcutConfig({});
});
afterEach(() => vi.unstubAllGlobals());

it('applies the persisted shortcut setting without writing it back', () => {
  applyShortcutConfig({ toggle_character_set_ctrl_shift_f: false });
  expect(characterSet.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
  applyShortcutConfig({ toggle_character_set_ctrl_shift_f: true });
  expect(characterSet.checked).toBe(true);
});

it('writes only the character-set shortcut setting when the user disables it', () => {
  setupShortcut();
  characterSet.checked = false;
  characterSet.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_character_set_ctrl_shift_f', false);
});

it('applies the persisted trilingual cycle switch without writing it back', () => {
  applyShortcutConfig({ trilingual_cycle: true });
  expect(trilingualCycle.checked).toBe(true);
  applyShortcutConfig({ trilingual_cycle: false });
  expect(trilingualCycle.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
});

it('writes only the trilingual cycle switch when the user toggles it', () => {
  setupShortcut();
  trilingualCycle.checked = true;
  trilingualCycle.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.trilingual_cycle', true);
});

it('accepts older configuration snapshots with no shortcut field', () => {
  applyShortcutConfig(undefined);
  applyShortcutConfig({ switch_language_shift: false });
  expect(characterSet.checked).toBe(true);
  expect(trilingualCycle.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
});

function key(code: string, options: Record<string, unknown> = {}): Event {
  const keyCode = /^Key[A-Z]$/.test(code) ? code.charCodeAt(3) : /^Digit[0-9]$/.test(code) ? code.charCodeAt(5) :
    /^F\d+$/.test(code) ? 111 + Number(code.slice(1)) : ({ Period: 190, ControlLeft: 17, AltLeft: 18,
      AltRight: 18, Escape: 27, Tab: 9 } as Record<string, number>)[code] ?? 0;
  const event = Object.assign(new Event('keydown', { cancelable: true }), {
    code, keyCode, location: 0, ctrlKey: true, altKey: false, shiftKey: false, metaKey: false, repeat: false,
    getModifierState: () => false, ...options,
  });
  editor.punctuationHotkeyButton.dispatchEvent(event);
  return event;
}

function confirmBinding(): void {
  editor.punctuationHotkeyConfirm.dispatchEvent(new Event('click'));
}

it('applies default, custom and explicitly disabled bindings without writing back', () => {
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  applyShortcutConfig({ toggle_punctuation: 'Alt+KeyA' });
  expect(editor.punctuationHotkeyButton.textContent).toBe('左 Alt + A');
  applyShortcutConfig({ toggle_punctuation: '' });
  expect(editor.punctuationHotkeyButton.textContent).toBe('未绑定');
  expect(updateConfig).not.toHaveBeenCalled();
});

it('records one new chord and waits for the persisted host snapshot', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('Period', { shiftKey: true });
  key('Period', { shiftKey: true, repeat: true });
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyStatus.textContent).toContain('Ctrl + Shift + .');
  expect(editor.punctuationHotkeyStatus.textContent).toContain('覆盖软件里的同名快捷键');
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(false);
  editor.punctuationHotkeyButton.dispatchEvent(new Event('blur'));
  confirmBinding();
  confirmBinding();
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+Shift+Period');
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  applyShortcutConfig({ toggle_punctuation: 'Ctrl+Shift+Period' });
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + Shift + .');
});

it('records Win32 keys with no scan code and cancels them with Escape', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('', { keyCode: 17 });
  key('', { keyCode: 16, shiftKey: true });
  expect(editor.punctuationHotkeyStatus.textContent).toContain('Esc');
  key('', { keyCode: 190, shiftKey: true });
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+Shift+Period');
  applyShortcutConfig({ toggle_punctuation: 'Ctrl+Shift+Period' });
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  expect(key('', { keyCode: 9 }).defaultPrevented).toBe(false);
  key('', { keyCode: 27 });
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + Shift + .');
  expect(editor.punctuationHotkeyButton.getAttribute('aria-pressed')).toBe('false');
  expect(updateConfig).toHaveBeenCalledTimes(1);
});

it('uses virtual keys with or without scan codes and rejects numpad aliases', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('', { keyCode: 70, shiftKey: true });
  key('', { keyCode: 190, altKey: true });
  key('NumpadDecimal', { keyCode: 190 });
  expect(updateConfig).not.toHaveBeenCalled();
  key('', { keyCode: 71 });
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+KeyG');
});

it.each(['punctuationHotkeyClear', 'punctuationHotkeyReset'])('writes the %s action', id => {
  setupShortcut();
  editor[id].dispatchEvent(new Event('click'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation',
    id === 'punctuationHotkeyClear' ? '' : 'Ctrl+Period');
});

it('preserves the confirmed value if saving fails and the host resends it', () => {
  setupShortcut();
  editor.punctuationHotkeyClear.dispatchEvent(new Event('click'));
  applyShortcutConfig({ toggle_punctuation: 'Ctrl+Period' });
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  expect(editor.punctuationHotkeyStatus.textContent).toBe('');
});

it('cancels on Escape or blur, leaves Tab navigation alone and ignores keys outside recording', () => {
  setupShortcut();
  expect(key('Period').defaultPrevented).toBe(false);
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('Escape');
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  expect(key('Tab').defaultPrevented).toBe(false);
  editor.punctuationHotkeyButton.dispatchEvent(new Event('blur'));
  key('Period');
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
});

it('rejects reserved, bare, Win and AltGr chords and ignores modifiers and repeats', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('ControlLeft');
  key('Period', { repeat: true });
  key('KeyF', { shiftKey: true });
  key('KeyE', { shiftKey: true });
  key('F9');
  key('F9', { shiftKey: true });
  key('AltLeft', { ctrlKey: false, altKey: true });
  key('F4', { ctrlKey: false, altKey: true });
  key('F4', { ctrlKey: false, altKey: true, shiftKey: true });
  key('Period', { ctrlKey: false });
  key('Period', { metaKey: true });
  key('Period', { altKey: true });
  key('Period', { getModifierState: () => true });
  expect(updateConfig).not.toHaveBeenCalled();
  key('AltLeft', { ctrlKey: false, altKey: true });
  key('Digit2', { ctrlKey: false, altKey: true });
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Alt+Digit2');
});

it.each([
  ['KeyQ', 65, 'KeyA'], // AZERTY: physical Q produces VK_A.
  ['KeyY', 90, 'KeyZ'], // QWERTZ: physical Y produces VK_Z.
  ['Semicolon', 188, 'Comma'], // OEM keys also follow the Windows VK.
])('records %s using its Windows virtual key', (code, keyCode, expected) => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key(code, { keyCode });
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', `Ctrl+${expected}`);
});

it.each([
  ['KeyE', { shiftKey: true }, '英文候选'],
  ['KeyF', { shiftKey: true }, '简繁'],
  ['F9', {}, '语音'],
  ['F9', { shiftKey: true }, '语音'],
  ['F4', { ctrlKey: false, altKey: true }, '关闭窗口'],
  ['F4', { ctrlKey: false, altKey: true, shiftKey: true }, '关闭窗口'],
])('names the feature that owns %s %j', (code, options, feature) => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('AltLeft', { ctrlKey: false, altKey: true });
  key(code, options);
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyStatus.textContent).toContain(feature);
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  confirmBinding();
  expect(updateConfig).not.toHaveBeenCalled();
});

it('requires left Alt to be pressed during recording and releases its state on keyup and blur', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  const alt = { ctrlKey: false, altKey: true };
  key('KeyG', alt); // Alt held before recording has no known side.
  key('AltRight', alt);
  key('KeyG', alt);
  key('AltLeft', alt); // Both sides down still belong to the right-Alt shortcut.
  key('KeyG', alt);
  expect(updateConfig).not.toHaveBeenCalled();
  editor.punctuationHotkeyButton.dispatchEvent(Object.assign(new Event('keyup'), {
    code: 'AltRight', keyCode: 18, location: 2, altKey: true,
  }));
  key('KeyG', alt);
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Alt+KeyG');
  vi.clearAllMocks();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('AltLeft', alt);
  editor.punctuationHotkeyButton.dispatchEvent(new Event('blur'));
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyG', alt);
  expect(updateConfig).not.toHaveBeenCalled();
});

it('records left Alt without a scan code when WebView2 supplies the key location', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('', { keyCode: 18, location: 1, ctrlKey: false, altKey: true });
  key('', { keyCode: 71, ctrlKey: false, altKey: true, shiftKey: true });
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Alt+Shift+KeyG');
});

it.each([
  ['KeyC', false, '复制'], ['KeyV', false, '粘贴'], ['KeyX', false, '剪切'],
  ['KeyA', false, '全选'], ['KeyZ', false, '撤销'], ['KeyY', false, '重做'],
  ['KeyS', false, '保存'], ['KeyF', false, '查找'], ['KeyH', false, '替换'],
  ['KeyP', false, '打印'], ['KeyO', false, '打开'], ['KeyN', false, '新建'],
  ['KeyW', false, '关闭'], ['F4', false, '关闭'], ['KeyT', false, '新建标签页'],
  ['KeyV', true, '粘贴为纯文本'], ['KeyZ', true, '重做'],
  ['KeyS', true, '另存为'], ['KeyT', true, '重新打开'],
] as const)('warns about %s Shift=%s (%s) before saving', (code, shiftKey, action) => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key(code, { shiftKey });
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  expect(editor.punctuationHotkeyStatus.textContent).toContain(action);
  expect(editor.punctuationHotkeyStatus.textContent).toContain('冲突');
  expect(editor.punctuationHotkeyStatus.textContent).toContain('中英文状态');
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(false);
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation',
    `Ctrl+${shiftKey ? 'Shift+' : ''}${code}`);
});

it.each(['cancel', 'escape', 'confirmation-escape'])('keeps the original binding after %s', action => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyC');
  if (action === 'cancel') editor.punctuationHotkeyCancel.dispatchEvent(new Event('click'));
  else if (action === 'escape') key('Escape');
  else editor.punctuationHotkeyConfirmation.dispatchEvent(Object.assign(new Event('keydown'), { key: 'Escape' }));
  confirmBinding();
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  expect(editor.punctuationHotkeyStatus.textContent).toBe('');
});

it('replaces a pending chord when recording starts again', () => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyC');
  key('KeyV'); // Keys outside recording cannot silently replace the pending chord.
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  confirmBinding();
  expect(updateConfig).not.toHaveBeenCalled();
  key('KeyG', { shiftKey: true });
  expect(editor.punctuationHotkeyStatus.textContent).toContain('Ctrl + Shift + G');
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+Shift+KeyG');
});

it.each(['punctuationHotkeyClear', 'punctuationHotkeyReset'])('discards a pending chord on %s', id => {
  setupShortcut();
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyC');
  editor[id].dispatchEvent(new Event('click'));
  confirmBinding();
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation',
    id === 'punctuationHotkeyClear' ? '' : 'Ctrl+Period');
});
