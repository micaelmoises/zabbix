<?php declare(strict_types = 0);
/*
** Zabbix
** Copyright (C) 2001-2023 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/


/**
 * @var CView $this
 * @var array $data
 */

$csrf_token = CCsrfTokenHelper::get('script');

$form = (new CForm())
	->addItem((new CVar(CCsrfTokenHelper::CSRF_TOKEN_NAME, $csrf_token))->removeId())
	->setId('script-form')
	->setName('scripts')
	->addVar('scriptid', $data['scriptid']);

$parameters_table = (new CTable())
	->setId('parameters-table')
	->setHeader([
		(new CColHeader(_('Name')))->setWidth('50%'),
		(new CColHeader(_('Value')))->setWidth('50%'),
		_('Action')
	])
	->setAttribute('style', 'width: 100%;')
	->addItem(
		(new CTag('tfoot', true))
			->addItem(
				(new CCol(
					(new CSimpleButton(_('Add')))
						->addClass(ZBX_STYLE_BTN_LINK)
						->addClass('js-parameter-add')
				))->setColSpan(2)
			)
	);

$row_template = (new CTemplateTag('script-parameter-template'))
	->addItem(
		(new CRow([
			(new CTextBox('parameters[name][]', '', false, DB::getFieldLength('script_param', 'name')))
				->setAttribute('style', 'width: 100%;')
				->setAttribute('value', '#{name}')
				->removeId(),
			(new CTextBox('parameters[value][]', '', false, DB::getFieldLength('script_param', 'value')))
				->setAttribute('style', 'width: 100%;')
				->setAttribute('value', '#{value}')
				->removeId(),
			(new CButton('', _('Remove')))
				->removeId()
				->addClass(ZBX_STYLE_BTN_LINK)
				->addClass('js-remove')
		]))->addClass('form_row')
	);

$form_grid = (new CFormGrid())
	->addItem([
		(new CLabel(_('Name'), 'name'))
			->setAsteriskMark(),
		(new CFormField(
			(new CTextBox('name', $data['name'], false, DB::getFieldLength('scripts', 'name')))
				->setWidth(ZBX_TEXTAREA_STANDARD_WIDTH)
				->setAriaRequired()
		))
	])
	->addItem(([
		new CLabel(_('Scope'), 'scope'),
		new CFormField(
			(new CRadioButtonList('scope', (int) $data['scope']))
				->addValue(_('Action operation'), ZBX_SCRIPT_SCOPE_ACTION)
				->addValue(_('Manual host action'), ZBX_SCRIPT_SCOPE_HOST)
				->addValue(_('Manual event action'), ZBX_SCRIPT_SCOPE_EVENT)
				->setModern()
				->setEnabled(!$data['actions']))
	]))
	->addItem([
		(new CLabel(_('Menu path'), 'menu_path'))->setId('menu-path-label'),
		(new CFormField(
			(new CTextBox('menu_path', $data['menu_path'], false, DB::getFieldLength('scripts', 'menu_path')))
				->setWidth(ZBX_TEXTAREA_STANDARD_WIDTH)
				->setAttribute('placeholder', _('<sub-menu/sub-menu/...>'))
		))->setId('menu-path')
	])
	->addItem([
		(new CLabel(_('Type'), 'type')),
		new CFormField(
			(new CRadioButtonList('type', (int) $data['type']))
				->addValue(_('URL'), ZBX_SCRIPT_TYPE_URL)
				->addValue(_('Webhook'), ZBX_SCRIPT_TYPE_WEBHOOK)
				->addValue(_('Script'), ZBX_SCRIPT_TYPE_CUSTOM_SCRIPT)
				->addValue(_('SSH'), ZBX_SCRIPT_TYPE_SSH)
				->addValue(_('Telnet'), ZBX_SCRIPT_TYPE_TELNET)
				->addValue(_('IPMI'), ZBX_SCRIPT_TYPE_IPMI)
				->setModern()
		)
	])
	->addItem([
		(new CLabel(_('Execute on'), 'execute_on'))->setId('execute-on-label'),
		(new CFormField(
			(new CRadioButtonList('execute_on', (int) $data['execute_on']))
				->addValue(_('Zabbix agent'), ZBX_SCRIPT_EXECUTE_ON_AGENT)
				->addValue(_('Zabbix server (proxy)'), ZBX_SCRIPT_EXECUTE_ON_PROXY)
				->addValue(_('Zabbix server'), ZBX_SCRIPT_EXECUTE_ON_SERVER)
				->setModern()
				->setId('execute-on')
		))->setId('execute-on')
	])
	->addItem([
		(new CLabel(_('Authentication method'), 'authentication'))->setId('auth-type-label'),
		(new CFormField(
			(new CSelect('authtype'))
				->setId('authtype')
				->setValue($data['authtype'])
				->setFocusableElementId('authentication')
				->addOptions(CSelect::createOptionsFromArray([
					ITEM_AUTHTYPE_PASSWORD => _('Password'),
					ITEM_AUTHTYPE_PUBLICKEY => _('Public key')
				]))
		))->setId('auth-type')
	])
	->addItem([
		(new CLabel(_('Username'), 'username'))
			->setId('username-label')
			->setAsteriskMark(),
		(new CFormField(
			(new CTextBox('username', $data['username'], false, DB::getFieldLength('scripts', 'username')))
				->setWidth(ZBX_TEXTAREA_SMALL_WIDTH)
				->setAriaRequired()
		))->setId('username-field')
	])
	->addItem([
		(new CLabel(_('Public key file'), 'publickey'))
			->setId('publickey-label')
			->setAsteriskMark(),
		(new CFormField(
			(new CTextBox('publickey', $data['publickey'], false, DB::getFieldLength('scripts', 'publickey')))
				->setWidth(ZBX_TEXTAREA_SMALL_WIDTH)
				->setAriaRequired()
		))->setId('publickey-field')
	])
	->addItem([
		(new CLabel(_('Private key file'), 'privatekey'))
			->setId('privatekey-label')
			->setAsteriskMark(),
		(new CFormField(
			(new CTextBox('privatekey', $data['privatekey'], false, DB::getFieldLength('scripts', 'privatekey')))
				->setWidth(ZBX_TEXTAREA_SMALL_WIDTH)
				->setAriaRequired()
		))->setId('privatekey-field')
	])
	->addItem([
		(new CLabel(_('Password'), 'password'))->setId('password-label'),
		(new CFormField(
			(new CTextBox('password', $data['password'], false, DB::getFieldLength('scripts', 'password')))
				->setWidth(ZBX_TEXTAREA_SMALL_WIDTH)
		))->setId('password-field')
	])
	->addItem([
		(new CLabel(_('Key passphrase'), 'passphrase'))->setId('passphrase-label'),
		(new CFormField(
			(new CTextBox('passphrase', $data['passphrase'], false, DB::getFieldLength('scripts', 'password')))
				->setWidth(ZBX_TEXTAREA_SMALL_WIDTH)
		))->setId('passphrase-field')
	])
	->addItem([
		(new CLabel(_('Port'), 'port'))->setId('port-label'),
		(new CFormField(
			(new CTextBox('port', $data['port'], false, DB::getFieldLength('scripts', 'port')))
				->setWidth(ZBX_TEXTAREA_SMALL_WIDTH)
		))->setId('port-field')
	])
	->addItem([
		(new CLabel(_('Commands'), 'command'))
			->setId('commands-label')
			->setAsteriskMark(),
		(new CFormField(
			(new CTextArea('command', $data['command']))
				->addClass(ZBX_STYLE_MONOSPACE_FONT)
				->setWidth(ZBX_TEXTAREA_STANDARD_WIDTH)
				->setMaxlength(DB::getFieldLength('scripts', 'command'))
				->setAriaRequired()
		))->setId('commands')
	])
	->addItem([
		(new CLabel(_('Command'), 'commandipmi'))
			->setId('command-ipmi-label')
			->setAsteriskMark(),
		(new CFormField(
			(new CTextBox('commandipmi', $data['commandipmi'], false, DB::getFieldLength('scripts', 'command')))
				->addClass(ZBX_STYLE_MONOSPACE_FONT)
				->setWidth(ZBX_TEXTAREA_STANDARD_WIDTH)
				->setAriaRequired()
		))->setId('command-ipmi')
	])
	->addItem([
		(new CLabel(_('Parameters'), $parameters_table->getId()))->setId('webhook-parameters-label'),
		(new CFormField(
			(new CDiv($parameters_table))
				->addClass(ZBX_STYLE_TABLE_FORMS_SEPARATOR)
				->setAttribute('style', 'min-width: '.ZBX_TEXTAREA_STANDARD_WIDTH.'px;')
		))->setId('webhook-parameters')
	])

	->addItem([
		(new CLabel(_('URL'), 'url'))
			->setId('url-label')
			->setAsteriskMark(),
		(new CFormField(
			(new CTextBox('url', $data['url'], false, DB::getFieldLength('scripts', 'url')))
				->setWidth(ZBX_TEXTAREA_STANDARD_WIDTH)
				->setAriaRequired()
		))->setId('url')
	])
	->addItem([
		(new CLabel(_('Open in a new window')))->setId('new-window-label'),
		(new CFormField(
			(new CCheckBox('new_window'))
				->setChecked($data['new_window'])
				->setUncheckedValue(ZBX_SCRIPT_URL_NEW_WINDOW_NO)
		))->setId('new-window')
	])
	->addItem([
		(new CLabel(_('Script'), 'script'))
			->setAsteriskMark()
			->setId('script-label'),
		(new CFormField(
			(new CMultilineInput('script', $data['script'], [
				'title' => _('JavaScript'),
				'placeholder' => _('script'),
				'placeholder_textarea' => 'return value',
				'grow' => 'auto',
				'rows' => 0,
				'maxlength' => DB::getFieldLength('scripts', 'command')
			]))
				->setWidth(ZBX_TEXTAREA_STANDARD_WIDTH)
				->setAriaRequired()
		))->setId('js-item-script-field')
	])
	->addItem([
		(new CLabel(_('Timeout'), 'timeout'))
			->setAsteriskMark()
			->setId('timeout-label'),
		(new CFormField(
			(new CTextBox('timeout', $data['timeout'], false, DB::getFieldLength('scripts', 'timeout')))
				->setWidth(ZBX_TEXTAREA_SMALL_WIDTH)
				->setAriaRequired()
		))->setId('timeout-field')
	])
	->addItem([
		(new CLabel(_('Description'), 'description')),
		new CFormField(
			(new CTextArea('description', $data['description']))
				->setWidth(ZBX_TEXTAREA_STANDARD_WIDTH)
				->setMaxlength(DB::getFieldLength('scripts', 'description'))
		)
	]);

$select_usrgrpid = (new CSelect('usrgrpid'))
	->setId('user-group')
	->setValue($data['usrgrpid'])
	->setFocusableElementId('usrgrpid')
	->addOption(new CSelectOption(0, _('All')))
	->addOptions(CSelect::createOptionsFromArray($data['usergroups']));

$select_hgstype = (new CSelect('hgstype'))
	->setId('hgstype-select')
	->setValue($data['hgstype'])
	->setFocusableElementId('hgstype')
	->addOption(new CSelectOption(0, _('All')))
	->addOption(new CSelectOption(1, _('Selected')));

$form_grid
	->addItem([
		new CLabel(_('Host group'), $select_hgstype->getFocusableElementId()),
		new CFormField($select_hgstype)
	])
	->addItem(
		(new CFormField(
			(new CMultiSelect([
				'name' => 'groupid',
				'object_name' => 'hostGroup',
				'multiple' => false,
				'data' => $data['hostgroup'],
				'popup' => [
					'parameters' => [
						'srctbl' => 'host_groups',
						'srcfld1' => 'groupid',
						'dstfrm' => $form->getName(),
						'dstfld1' => 'groupid'
					]
				]
			]))
				->setWidth(ZBX_TEXTAREA_STANDARD_WIDTH)
		))->setId('host-group-selection')
	)
	->addItem([
		(new CLabel(_('User group'), $select_usrgrpid->getFocusableElementId()))->setId('usergroup-label'),
		(new CFormField($select_usrgrpid))->setId('usergroup')
	])
	->addItem([
		(new CLabel(_('Required host permissions'), 'host_access'))->setId('host-access-label'),
		(new CFormField(
			(new CRadioButtonList('host_access', (int) $data['host_access']))
				->addValue(_('Read'), PERM_READ)
				->addValue(_('Write'), PERM_READ_WRITE)
				->setModern()
				->setId('host-access')
		))->setId('host-access-field')
	])
	->addItem([
		(new CLabel(_('Enable confirmation'), 'enable-confirmation'))->setId('enable-confirmation-label'),
		(new CFormField(
			(new CCheckBox('enable_confirmation'))
				->setChecked($data['enable_confirmation'])
				->setId('enable-confirmation')
		))->setId('enable-confirmation-field')
	])
	->addItem([
		(new CLabel(_('Confirmation text'), 'confirmation'))->setId('confirmation-label'),
		(new CFormField([
			(new CTextBox('confirmation', $data['confirmation'], false, DB::getFieldLength('scripts', 'confirmation')))
				->setAttribute('disabled', $data['enable_confirmation'])
				->setWidth(ZBX_TEXTAREA_STANDARD_WIDTH),
			NBSP(),
			(new CButton('testConfirmation', _('Test confirmation')))
				->addClass(ZBX_STYLE_BTN_GREY)
				->setAttribute('disabled', $data['enable_confirmation'])
				->setId('test-confirmation')
		]))->setId('confirmation-field')
	]);

if ($data['scriptid'] === null) {
	$buttons = [
		[
			'title' => _('Add'),
			'keepOpen' => true,
			'isSubmit' => true,
			'action' => 'script_edit_popup.submit();'
		]
	];
}
else {
	$buttons = [
		[
			'title' => _('Update'),
			'keepOpen' => true,
			'isSubmit' => true,
			'action' => 'script_edit_popup.submit();'
		],
		[
			'title' => _('Clone'),
			'class' => ZBX_STYLE_BTN_ALT, 'js-clone',
			'keepOpen' => true,
			'isSubmit' => false,
			'action' => 'script_edit_popup.clone('.json_encode([
				'title' => _('New script'),
				'buttons' => [
					[
						'title' => _('Add'),
						'class' => 'js-add',
						'keepOpen' => true,
						'isSubmit' => true,
						'action' => 'script_edit_popup.submit();'
					],
					[
						'title' => _('Cancel'),
						'class' => ZBX_STYLE_BTN_ALT,
						'cancel' => true,
						'action' => ''
					]
				]
			]).');'
		],
		[
			'title' => _('Delete'),
			'confirmation' => _('Delete script?'),
			'class' => ZBX_STYLE_BTN_ALT,
			'keepOpen' => true,
			'isSubmit' => false,
			'action' => 'script_edit_popup.delete();'
		]
	];
}

$form
	->addItem($form_grid)
	->addItem($row_template)
	->addItem(
		(new CScriptTag('script_edit_popup.init('.json_encode([
			'script' => $data
		]).');'))->setOnDocumentReady()
	)
	->addStyle('display: none;');

$output = [
	'header' => $data['scriptid'] === null ? _('New script') : _('Script'),
	'doc_url' => CDocHelper::getUrl(CDocHelper::ALERTS_SCRIPT_EDIT),
	'body' => $form->toString(),
	'buttons' => $buttons,
	'script_inline' => getPagePostJs().$this->readJsFile('administration.script.edit.js.php')
];

if ($data['user']['debug_mode'] == GROUP_DEBUG_MODE_ENABLED) {
	CProfiler::getInstance()->stop();
	$output['debug'] = CProfiler::getInstance()->make()->toString();
}

echo json_encode($output);
