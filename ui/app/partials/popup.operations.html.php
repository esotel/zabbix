<?php declare(strict_types = 0);
/*
** Zabbix
** Copyright (C) 2001-2022 Zabbix SIA
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
 * @var CPartial $this
 * @var array $data
 */


if ($data['table'] === 'operation') {
	// Create operations table.
	$operations_table = (new CTable())
		->setId('operation-table')
		->setAttribute('style', 'width: 100%;');

	if (!array_key_exists('action', $data)) {
		$operations = $data['operations'];
		$eventsource = array_key_exists('eventsource', $operations[0])
			? $operations[0]['eventsource']
			: $data['eventsource'];
	}
	else {
		$operations = $data['action']['operations'];
		$eventsource = $data['eventsource'];
	}

	if (in_array($eventsource, [EVENT_SOURCE_TRIGGERS, EVENT_SOURCE_INTERNAL, EVENT_SOURCE_SERVICE])) {
		$operations_table->setHeader([_('Steps'), _('Details'), _('Start in'), _('Duration'), _('Action')]);
	}
	else {
		$operations_table->setHeader([_('Details'), _('Action')]);
	}

	foreach ($operations as $operationid => $operation) {
		if (in_array($eventsource, [EVENT_SOURCE_TRIGGERS, EVENT_SOURCE_INTERNAL, EVENT_SOURCE_SERVICE])) {
			$simple_interval_parser = new CSimpleIntervalParser();

			// todo : fix delays when opening action edit popup and editing step duration

			$delays = array_key_exists('action', $data)
				? count_operations_delay($data['action']['operations'], $data['action']['esc_period'])
				: count_operations_delay($operations, $data['esc_period']);

			$esc_steps_txt = null;
			$esc_period_txt = null;
			$esc_delay_txt = null;

			if ($operation['esc_step_from'] < 1) {
				$operation['esc_step_from'] = 1;
			}

			// display N-N as N
			$esc_steps_txt = ($operation['esc_step_from'] == $operation['esc_step_to'] || $operation['esc_step_to'] == 0)
				? $operation['esc_step_from']
				: $operation['esc_step_from'].' - '.$operation['esc_step_to'];

			$esc_period_txt = ($simple_interval_parser->parse($operation['esc_period']) == CParser::PARSE_SUCCESS
				&& timeUnitToSeconds($operation['esc_period']) == 0)
				? _('Default')
				: $operation['esc_period'];

			$esc_delay_txt = ($delays[$operation['esc_step_from']] === null)
				? _('Unknown')
				: ($delays[$operation['esc_step_from']] != 0
					? convertUnits(['value' => $delays[$operation['esc_step_from']], 'units' => 'uptime'])
					: _('Immediately')
				);
		}

		// todo : remove details and other unnecessarry data from hidden inputs

		$buttons =
			(new CHorList([
				(new CSimpleButton(_('Edit')))
					->addClass(ZBX_STYLE_BTN_LINK)
					->addClass('js-edit-operation')
					->setAttribute('data_operation', json_encode([
						'operationid' => $operationid,
						'actionid' => array_key_exists('actionid', $data) ? $data['actionid'] : 0,
						'eventsource' => $eventsource,
						'operationtype' => $operation['operationtype'],
						'data' => $operation
					])),
				[
					(new CButton('remove', _('Remove')))
						->setAttribute('data_operationid', $operationid)
						->addClass('js-remove')
						->addClass(ZBX_STYLE_BTN_LINK)
						->removeId(),
					new CVar('operations['.$operationid.']', $operation),
					//	new CVar('operations_for_popup['.ACTION_UPDATE_OPERATION.']['.$operationid.']',
					//		json_encode($operation_for_popup)
					//	)
				]
			]))
				->setName('button-list')
				->addClass(ZBX_STYLE_NOWRAP);


		// todo: add all data to rows
		// todo : fix if two or three types of data

		$details_column = new CCol([
			new CTag('b', true, $operation['details']['type'][0]),
			implode(' ', $operation['details']['data'][0])

		]);

		if (in_array($eventsource, [EVENT_SOURCE_TRIGGERS, EVENT_SOURCE_INTERNAL, EVENT_SOURCE_SERVICE])) {
			$operations_table->addRow([
				$esc_steps_txt,
				$details_column,
				$esc_delay_txt,
				$operation['esc_period'] == 0 ? 'Default' : $operation['esc_period'],
				$buttons
			]);
		}
		else {
			$operations_table->addRow([
				$details_column,
				$buttons
			]);
		}
	}

	$operations_table->addItem(
		(new CTag('tfoot', true))
			->addItem(
				(new CCol(
					(new CSimpleButton(_('Add')))
						->setAttribute('data-actionid', array_key_exists('actionid', $data) ? $data['actionid'] : 0)
						->setAttribute('data-eventsource', $eventsource)
						->setAttribute('operationtype', ACTION_OPERATION)
						->addClass('js-operation-details')
						->addClass(ZBX_STYLE_BTN_LINK)
				))->setColSpan(4)
			)
	);

	$operations_table->show();
}


// Create operations recovery table.
elseif ($data['table'] === 'recovery') {
	$operations_table = (new CTable())
		->setId('recovery-table')
		->setAttribute('style', 'width: 100%;');

// todo : pass data in 'action' array, so that the code can be cleaned a bit
	if (!array_key_exists('action', $data)) {
		$operations = $data['operations'];
	}
	else {
		$operations = $data['action']['recovery_operations'];
	}

		$operations_table->setHeader([_('Details'), _('Action')]);

		if ($operations) {
			foreach ($operations as $operationid => $operation) {
				// todo : add check for allowed operations
				if (!isset($operation['opconditions'])) {
					$operation['opconditions'] = [];
				}
				if (!array_key_exists('opmessage', $operation)) {
					$operation['opmessage'] = [];
				}
				$operation['opmessage'] += [
					'mediatypeid' => '0',
					'message' => '',
					'subject' => '',
					'default_msg' => '1'
				];

				$operation_for_popup = array_merge($operation, ['id' => $operationid]);

				foreach (['opcommand_grp' => 'groupid', 'opcommand_hst' => 'hostid'] as $var => $field) {
					if (array_key_exists($var, $operation_for_popup)) {
						$operation_for_popup[$var] = zbx_objectValues($operation_for_popup[$var], $field);
					}
				}

				$details_column = new CCol([
					// todo : fix if two or three types of data
					new CTag('b', true, $operation['details']['type'][0]),
					implode(' ', $operation['details']['data'][0])

				]);

				$operations_table->addRow([
					$details_column,
					(new CCol(
						new CHorList([
							(new CSimpleButton(_('Edit')))
								->addClass(ZBX_STYLE_BTN_LINK)
								->addClass('js-edit-operation')
								->setAttribute('data_operation', json_encode([
									'operationid' => $operationid,
									'actionid' => array_key_exists('actionid', $data) ? $data['actionid'] : 0,
									'eventsource' => array_key_exists('eventsource', $data)
										? $data['eventsource']
										: $operation['eventsource'],
									'operationtype' => ACTION_RECOVERY_OPERATION,
									'data' => $operation
								])),
							[
								(new CButton('remove', _('Remove')))
									->setAttribute('data_operationid', $operationid)
									->addClass('js-remove')
									->addClass(ZBX_STYLE_BTN_LINK)
									->removeId(),
								new CVar('recovery_operations[' . $operationid . ']', $operation),
							// todo : check if this is necessary
//							new CVar('operations_for_popup['.ACTION_RECOVERY_OPERATION.']['.$operationid.']',
//								json_encode($operation_for_popup)
//							)
							]
						])
					))->addClass(ZBX_STYLE_NOWRAP)
				], null, 'recovery_operations_' . $operationid);
			}
		}

		$operations_table->addItem(
			(new CTag('tfoot', true))
				->addItem(
					(new CCol(
						(new CSimpleButton(_('Add')))
							->setAttribute('operationtype', ACTION_RECOVERY_OPERATION)
							->setAttribute('data-actionid', array_key_exists('actionid', $data) ? $data['actionid'] : 0)
							->setAttribute('data-eventsource', array_key_exists('eventsource', $data)
								? $data['eventsource']
								: $operation['eventsource']
							)
							->addClass('js-recovery-operations-create')
							->addClass(ZBX_STYLE_BTN_LINK)
					))->setColSpan(4)
				)
		);
		$operations_table->show();
}
elseif ($data['table'] === 'update') {
	if (!array_key_exists('action', $data)) {
		$operations = $data['operations'];
	}
	else {
		$operations = $data['action']['update_operations'];
	}

	$operations_table = (new CTable())
			->setId('update-table')
			->setAttribute('style', 'width: 100%;')
			->setHeader([_('Details'), _('Action')]);

		if ($operations) {
			foreach ($operations as $operationid => $operation) {
				// todo : pass allowed operations
//				if (!str_in_array($operation['operationtype'], $data['allowedOperations'][ACTION_UPDATE_OPERATION])) {
//					continue;
//				}
				$operation += [
					'opconditions' => []
				];
				// $details = new CSpan($operation_descriptions[0][$operationid]);
				$operation_for_popup = array_merge($operation, ['id' => $operationid]);
				foreach (['opcommand_grp' => 'groupid', 'opcommand_hst' => 'hostid'] as $var => $field) {
					if (array_key_exists($var, $operation_for_popup)) {
						$operation_for_popup[$var] = zbx_objectValues($operation_for_popup[$var], $field);
					}
				}

				$details_column = new CCol([
					// todo : fix if two or three types of data
					new CTag('b', true, $operation['details']['type'][0]),
					implode(' ', $operation['details']['data'][0])

				]);

				$operations_table->addRow([
					$details_column,
					(new CCol(
						new CHorList([
							(new CSimpleButton(_('Edit')))
								->addClass(ZBX_STYLE_BTN_LINK)
								->addClass('js-edit-operation')
								->setAttribute('data_operation', json_encode([
									'operationid' => $operationid,
									'actionid' => array_key_exists('actionid', $data) ? $data['actionid'] : 0,
									'eventsource' => array_key_exists('eventsource', $data)
										? $data['eventsource']
										: $operation['eventsource'],
									'operationtype' => ACTION_UPDATE_OPERATION,
									'data' => $operation
								])),
							[
								(new CButton('remove', _('Remove')))
									->setAttribute('data_operationid', $operationid)
									->addClass('js-remove')
									->addClass(ZBX_STYLE_BTN_LINK)
									->removeId(),
								new CVar('update_operations['.$operationid.']', $operation),
								new CVar('operations_for_popup['.ACTION_UPDATE_OPERATION.']['.$operationid.']',
									json_encode($operation_for_popup)
								)
							]
						])
					))->addClass(ZBX_STYLE_NOWRAP)
				], null, 'update_operations_'.$operationid);
			}
		}

	$operations_table->addItem(
			(new CTag('tfoot', true))
				->addItem(
					(new CCol(
						(new CSimpleButton(_('Add')))
							->setAttribute('data-actionid', array_key_exists('actionid', $data) ? $data['actionid'] : 0)
							->setAttribute('operationtype', ACTION_UPDATE_OPERATION)
							->setAttribute('data-eventsource', array_key_exists('eventsource', $data)
								? $data['eventsource']
								: $operation['eventsource'])
							->addClass('js-update-operations-create')
							->addClass(ZBX_STYLE_BTN_LINK)
					))->setColSpan(4)
				)
		);

	$operations_table->show();
}
